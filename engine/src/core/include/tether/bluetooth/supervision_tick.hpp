#pragma once

namespace tether::bluetooth {

    // The steps of one supervision tick. They all run on one thread, so the
    // order is what decides how long a returning phone waits for each service.
    class SupervisionSteps {
    public:
        virtual ~SupervisionSteps() = default;

        // Bearer supervisor, and the resets a dropped link calls for. Never blocks.
        virtual void links() = 0;
        // ANCS subscription and queued notification packets. Rides LE alone.
        virtual void notifications() = 0;
        // Calls in progress.
        virtual void calls() = 0;
        // Status to the subscribed clients; a payload equal to the last is dropped.
        virtual void publish() = 0;
        // OBEX sessions. Opening one can wait on obexd for tens of seconds.
        // True when it held the thread or changed a session: the link may no
        // longer be what links() saw.
        virtual bool profiles() = 0;
        // Away lock and the LE solicitation.
        virtual void presence() = 0;
        // Whether the long reads below may run in this tick. False while the
        // notification session is being rebuilt: see NotificationPriority.
        virtual bool long_reads_allowed() = 0;
        // Address book, call history, mailbox: long OBEX reads. True when the
        // step held the thread, false when it had nothing to do.
        virtual bool contacts() = 0;
        virtual bool call_history() = 0;
        virtual bool messages() = 0;
    };

    // What must never wait: the links as they are now, notifications, calls,
    // and the status that results, published.
    inline void run_quick_steps(SupervisionSteps& steps) {
        steps.links();
        steps.notifications();
        steps.calls();
        steps.publish();
    }

    // Notifications and calls are restored, and the result published, before
    // anything that can wait on OBEX. They used to run last, behind a full
    // address-book and mailbox read, which is where a returning phone lost
    // most of the time before its notifications came back. Running first in
    // the tick is not enough: the ANCS session comes back over several ticks,
    // so the long reads are skipped altogether while it does.
    //
    // The published status is built from what links() last saw. Whenever a step
    // has held the thread, the quick steps run again before the next one, so a
    // change is published when it is known and not a tick and a long read
    // later: a BR/EDR link that obexd brought up was published five seconds
    // after the session riding on it.
    inline void run_supervision_tick(SupervisionSteps& steps) {
        run_quick_steps(steps);

        if (steps.profiles())
            steps.links();
        steps.presence();
        steps.publish();

        if (!steps.long_reads_allowed())
            return;
        if (steps.contacts())
            run_quick_steps(steps);
        if (steps.call_history())
            run_quick_steps(steps);
        if (steps.messages())
            run_quick_steps(steps);
    }

} // namespace tether::bluetooth
