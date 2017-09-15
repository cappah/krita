/*
 *  Copyright (c) 2017 Dmitry Kazakov <dimula73@gmail.com>
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 */

#include "KisDabRenderingQueue.h"

#include "KisDabRenderingJob.h"
#include "KisRenderedDab.h"
#include "KisSharedThreadPoolAdapter.h"

struct KisDabRenderingQueue::Private
{
    struct JobWrapper {
        enum Status {
            New,
            Running,
            Completed
        };

        KisDabRenderingJob job;
        Status status = New;
    };

    struct DumbCacheInterface : public CacheInterface {
        virtual void getDabType(bool hasDabInCache,
                                KisDabCacheUtils::DabRenderingResources *resources,
                                const KisDabCacheUtils::DabRequestInfo &request,
                                /* out */
                                KisDabCacheUtils::DabGenerationInfo *di,
                                bool *shouldUseCache)
        {
            Q_UNUSED(hasDabInCache);
            Q_UNUSED(resources);
            Q_UNUSED(request);

            di->needsPostprocessing = false;
            *shouldUseCache = false;
        }

    };

    Private(const KoColorSpace *_colorSpace,
            KisDabCacheUtils::ResourcesFactory _resourcesFactory,
            KisSharedThreadPoolAdapter *_sharedThreadPool)
        : cacheInterface(new DumbCacheInterface),
          colorSpace(_colorSpace),
          sharedThreadPool(_sharedThreadPool),
          resourcesFactory(_resourcesFactory)
    {
        KIS_SAFE_ASSERT_RECOVER_NOOP(resourcesFactory);
    }

    ~Private() {
        qDeleteAll(cachedResources);
        cachedResources.clear();
    }

    QList<JobWrapper> jobs;
    int startSeqNo = 0;
    int lastPaintedJob = -1;
    QScopedPointer<CacheInterface> cacheInterface;
    const KoColorSpace *colorSpace;
    KisSharedThreadPoolAdapter *sharedThreadPool;

    KisDabCacheUtils::ResourcesFactory resourcesFactory;
    QList<KisDabCacheUtils::DabRenderingResources*> cachedResources;
    QList<KisFixedPaintDeviceSP> cachedPaintDevices;

    int findLastDabJobIndex(int startSearchIndex = -1);
    KisDabRenderingJob* createPostprocessingJob(const KisDabRenderingJob &postprocessingJob, int sourceDabJob);
    void cleanPaintedDabs();
};


KisDabRenderingQueue::KisDabRenderingQueue(const KoColorSpace *cs,
                                           KisDabCacheUtils::ResourcesFactory resourcesFactory,
                                           KisSharedThreadPoolAdapter *sharedThreadPool)
    : m_d(new Private(cs, resourcesFactory, sharedThreadPool))
{
}

KisDabRenderingQueue::~KisDabRenderingQueue()
{
}

int KisDabRenderingQueue::Private::findLastDabJobIndex(int startSearchIndex)
{
    if (startSearchIndex < 0) {
        startSearchIndex = jobs.size() - 1;
    }

    for (int i = startSearchIndex; i >= 0; i--) {
        if (jobs[i].job.type == KisDabRenderingJob::Dab) {
            return i;
        }
    }

    return -1;
}

KisDabRenderingJob *KisDabRenderingQueue::Private::createPostprocessingJob(const KisDabRenderingJob &postprocessingJob, int sourceDabJob)
{
    KisDabRenderingJob *job = new KisDabRenderingJob(postprocessingJob);
    job->originalDevice = jobs[sourceDabJob].job.originalDevice;
    return job;
}

KisDabRenderingJob *KisDabRenderingQueue::addDab(const KisDabCacheUtils::DabRequestInfo &request)
{
    const int seqNo =
        !m_d->jobs.isEmpty() ?
            m_d->jobs.last().job.seqNo + 1:
            m_d->startSeqNo;

    KisDabCacheUtils::DabRenderingResources *resources = 0;

    if (!m_d->cachedResources.isEmpty()) {
        resources = m_d->cachedResources.takeLast();
    } else {
        resources = m_d->resourcesFactory();
    }

    KIS_SAFE_ASSERT_RECOVER_RETURN_VALUE(resources, 0);

    // We should sync the cached brush into the current seqNo
    resources->syncResourcesToSeqNo(seqNo);

    const int lastDabJobIndex = m_d->findLastDabJobIndex();

    Private::JobWrapper wrapper;

    bool shouldUseCache = false;
    m_d->cacheInterface->getDabType(lastDabJobIndex >= 0,
                                    resources,
                                    request,
                                    &wrapper.job.generationInfo,
                                    &shouldUseCache);

    // TODO: initialize via c-tor
    wrapper.job.seqNo = seqNo;
    wrapper.job.resources = resources;
    wrapper.job.type =
        !shouldUseCache ? KisDabRenderingJob::Dab :
        wrapper.job.generationInfo.needsPostprocessing ? KisDabRenderingJob::Postprocess :
        KisDabRenderingJob::Copy;
    wrapper.job.parentQueue = this;
    wrapper.job.sharedThreadPool = m_d->sharedThreadPool;

    KisDabRenderingJob *jobToRun = 0;

    if (wrapper.job.type == KisDabRenderingJob::Dab) {
        jobToRun = new KisDabRenderingJob(wrapper.job);
        wrapper.status = Private::JobWrapper::Running;
    } else if (wrapper.job.type == KisDabRenderingJob::Postprocess ||
               wrapper.job.type == KisDabRenderingJob::Copy) {

        KIS_SAFE_ASSERT_RECOVER_RETURN_VALUE(lastDabJobIndex >= 0, 0);

        if (m_d->jobs[lastDabJobIndex].status == Private::JobWrapper::Completed) {
            if (wrapper.job.type == KisDabRenderingJob::Postprocess) {
                jobToRun = m_d->createPostprocessingJob(wrapper.job, lastDabJobIndex);
                wrapper.status = Private::JobWrapper::Running;
                wrapper.job.originalDevice = m_d->jobs[lastDabJobIndex].job.originalDevice;
            } else if (wrapper.job.type == KisDabRenderingJob::Copy) {
                wrapper.status = Private::JobWrapper::Completed;
                wrapper.job.originalDevice = m_d->jobs[lastDabJobIndex].job.originalDevice;
                wrapper.job.postprocessedDevice = m_d->jobs[lastDabJobIndex].job.postprocessedDevice;
            }
        }
    }

    m_d->jobs.append(wrapper);

    KIS_SAFE_ASSERT_RECOVER_NOOP(m_d->startSeqNo ==
                                 m_d->jobs.first().job.seqNo);

    if (wrapper.job.type == KisDabRenderingJob::Dab) {
        m_d->cleanPaintedDabs();
    }

    return jobToRun;
}

QList<KisDabRenderingJob *> KisDabRenderingQueue::notifyJobFinished(KisDabRenderingJob *job)
{
    QList<KisDabRenderingJob *> dependentJobs;

    const int jobIndex = job->seqNo - m_d->startSeqNo;
    KIS_SAFE_ASSERT_RECOVER_RETURN_VALUE(
        jobIndex >= 0 && jobIndex < m_d->jobs.size(), dependentJobs);


    Private::JobWrapper &wrapper = m_d->jobs[jobIndex];
    KisDabRenderingJob &finishedJob = wrapper.job;

    KIS_SAFE_ASSERT_RECOVER_NOOP(m_d->jobs[jobIndex].status == Private::JobWrapper::Running);
    KIS_SAFE_ASSERT_RECOVER_NOOP(finishedJob.resources == job->resources);
    KIS_SAFE_ASSERT_RECOVER_NOOP(finishedJob.seqNo == job->seqNo);
    KIS_SAFE_ASSERT_RECOVER_NOOP(finishedJob.type == job->type);
    KIS_SAFE_ASSERT_RECOVER_NOOP(job->originalDevice);
    KIS_SAFE_ASSERT_RECOVER_NOOP(job->postprocessedDevice);

    wrapper.status = Private::JobWrapper::Completed;
    finishedJob.originalDevice = job->originalDevice;
    finishedJob.postprocessedDevice = job->postprocessedDevice;

    // recycle the resources
    m_d->cachedResources << finishedJob.resources;
    finishedJob.resources = 0;
    job->resources = 0;

    if (finishedJob.type == KisDabRenderingJob::Dab) {
        for (int i = jobIndex + 1; i < m_d->jobs.size(); i++) {
            Private::JobWrapper &w = m_d->jobs[i];
            KisDabRenderingJob &j = w.job;

            // next dab job closes the chain
            if (j.type == KisDabRenderingJob::Dab) break;

            // the non 'dab'-type job couldn't have
            // been started before the source ob was completed
            KIS_SAFE_ASSERT_RECOVER_BREAK(w.status == Private::JobWrapper::New);

            if (j.type == KisDabRenderingJob::Copy) {
                j.originalDevice = job->originalDevice;
                j.postprocessedDevice = job->postprocessedDevice;
                w.status = Private::JobWrapper::Completed;
            } else if (j.type == KisDabRenderingJob::Postprocess) {
                dependentJobs << m_d->createPostprocessingJob(j, jobIndex);
                w.status = Private::JobWrapper::Running;
            }
        }
    }

    return dependentJobs;
}

void KisDabRenderingQueue::Private::cleanPaintedDabs()
{
    const int nextToBePainted = lastPaintedJob + 1;
    const int sourceJob = findLastDabJobIndex(qMin(nextToBePainted, jobs.size() - 1));

    if (sourceJob >= 1) {
        // recycle and remove first 'sourceJob' jobs

        for (auto it = jobs.begin(); it != jobs.begin() + sourceJob; ++it) {
            if (it->job.postprocessedDevice != it->job.originalDevice) {
                cachedPaintDevices << it->job.originalDevice;
                it->job.originalDevice = 0;
            }
        }

        jobs.erase(jobs.begin(), jobs.begin() + sourceJob);

        KIS_SAFE_ASSERT_RECOVER_RETURN(jobs.size() > 0);

        lastPaintedJob -= sourceJob;
        startSeqNo = jobs.first().job.seqNo;
    }
}

QList<KisRenderedDab> KisDabRenderingQueue::takeReadyDabs()
{
    QList<KisRenderedDab> renderedDabs;
    if (m_d->startSeqNo < 0) return renderedDabs;

    KIS_SAFE_ASSERT_RECOVER_NOOP(
        m_d->jobs.isEmpty() ||
        m_d->jobs.first().job.type == KisDabRenderingJob::Dab);

    for (int i = 0; i < m_d->jobs.size(); i++) {
        Private::JobWrapper &w = m_d->jobs[i];
        KisDabRenderingJob &j = w.job;

        if (w.status != Private::JobWrapper::Completed) break;

        if (i <= m_d->lastPaintedJob) continue;

        KisRenderedDab dab;

        dab.device = j.postprocessedDevice;
        dab.offset = j.dstDabOffset();

        renderedDabs.append(dab);

        m_d->lastPaintedJob = i;
    }

    m_d->cleanPaintedDabs();
    return renderedDabs;
}

bool KisDabRenderingQueue::hasPreparedDabs() const
{
    const int nextToBePainted = m_d->lastPaintedJob + 1;

    return
        nextToBePainted >= 0 &&
        nextToBePainted < m_d->jobs.size() &&
            m_d->jobs[nextToBePainted].status == Private::JobWrapper::Completed;
}

void KisDabRenderingQueue::setCacheInterface(KisDabRenderingQueue::CacheInterface *interface)
{
    m_d->cacheInterface.reset(interface);
}

KisFixedPaintDeviceSP KisDabRenderingQueue::fetchCachedPaintDevce()
{
    return
        m_d->cachedPaintDevices.isEmpty() ?
            new KisFixedPaintDevice(m_d->colorSpace) :
            m_d->cachedPaintDevices.takeLast();
}

int KisDabRenderingQueue::testingGetQueueSize() const
{
    return m_d->jobs.size();
}

