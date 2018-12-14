#include <vector>
#include <thread>
#include <boost/optional.hpp>

#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/Channel.hpp"
#include "OctoPrint.hpp"
#include "Duet.hpp"
#include "../GUI/PrintHostDialogs.hpp"

using boost::optional;


namespace Slic3r {


PrintHost::~PrintHost() {}

PrintHost* PrintHost::get_print_host(DynamicPrintConfig *config)
{
    PrintHostType kind = config->option<ConfigOptionEnum<PrintHostType>>("host_type")->value;
    if (kind == htOctoPrint) {
        return new OctoPrint(config);
    } else if (kind == htDuet) {
        return new Duet(config);
    }
    return nullptr;
}


struct PrintHostJobQueue::priv
{
    // XXX: comment on how bg thread works

    PrintHostJobQueue *q;

    Channel<PrintHostJob> channel_jobs;
    Channel<size_t> channel_cancels;
    size_t job_id = 0;

    std::thread bg_thread;
    bool bg_exit;

    GUI::PrintHostQueueDialog *queue_dialog;

    priv(PrintHostJobQueue *q) : q(q) {}

    void start_bg_thread();
    void bg_thread_main();
    void perform_job(PrintHostJob the_job);
};

PrintHostJobQueue::PrintHostJobQueue(GUI::PrintHostQueueDialog *queue_dialog)
    : p(new priv(this))
{
    p->queue_dialog = queue_dialog;
}

PrintHostJobQueue::~PrintHostJobQueue()
{
    if (p && p->bg_thread.joinable()) {
        p->bg_exit = true;
        p->channel_jobs.push(PrintHostJob()); // Push an empty job to wake up bg_thread in case it's sleeping
        p->bg_thread.detach();                // Let the background thread go, it should exit on its own
    }
}

void PrintHostJobQueue::priv::start_bg_thread()
{
    if (bg_thread.joinable()) { return; }

    std::shared_ptr<priv> p2 = q->p;
    bg_thread = std::thread([p2]() {
        p2->bg_thread_main();
    });
}

void PrintHostJobQueue::priv::bg_thread_main()
{
    // bg thread entry point

    // Pick up jobs from the job channel:
    while (! bg_exit) {
        auto job = channel_jobs.pop();   // Sleeps in a cond var if there are no jobs
        if (! job.cancelled) {
            perform_job(std::move(job));
        }
        job_id++;
    }
}

void PrintHostJobQueue::priv::perform_job(PrintHostJob the_job)
{
    if (bg_exit || the_job.empty()) { return; }

    the_job.printhost->upload(std::move(the_job.upload_data), [this](Http::Progress progress, bool &cancel) {
        if (bg_exit) {
            cancel = true;
            return;
        }

        if (channel_cancels.size_hint() > 0) {
            // Lock both queues
            auto cancels = std::move(channel_cancels.lock_rw());
            auto jobs = std::move(channel_jobs.lock_rw());

            for (size_t cancel_id : *cancels) {
                if (cancel_id == job_id) {
                    cancel = true;
                } else if (cancel_id > job_id) {
                    jobs->at(cancel_id - job_id).cancelled = true;
                }
            }

            cancels->clear();
        }

        // TODO: report progress
    });
}

void PrintHostJobQueue::enqueue(PrintHostJob job)
{
    p->start_bg_thread();
    p->queue_dialog->append_job(job);
    // p->channel_jobs.push(std::move(job));    // XXX
}


}
