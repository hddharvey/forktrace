#include <string>
#include <unistd.h>
#include <cassert>
#include <iostream>
#include <cstring>
#include <system_error>
#include <fmt/core.h>
#include <sys/wait.h>

#include "tracer.hpp"
#include "process.hpp"
#include "util.hpp"
#include "ptrace.hpp"

using std::string;
using std::string_view;
using std::vector;
using std::shared_ptr;
using std::unique_ptr;
using std::system_error;
using std::generic_category;
using fmt::format;

BadTraceError::BadTraceError(pid_t pid, string_view msg) : _pid(pid)
{
    _message = format("BadTraceError (pid={}): {}", pid, msg);
}

BadTraceError::BadTraceError(pid_t pid, int wstatus, string_view msg)
    : _pid(pid)
{
    _message = format("BadTraceError (pid={}, wstatus={}): {}", 
        pid, wstatus, msg);
}

/* Okay this is really weird but this method should really be in the tracer
 * class. I don't want to do that because then I'd expose this method to
 * everyone which I don't want to do. Instead I made the Tracee class a
 * friend of the Tracer and I'll put this static member function inside it. */
void Tracee::remove_tracee(Tracer& tracer, pid_t pid)
{
    tracer._tracees.erase(pid);
}

/* A sub-class of BlockingCall specialised for wait calls (wait4 or waitid).
 * This class does a bunch of trickery to obtain the result of the wait call,
 * even when the tracee program specifies 'NULL' as the argument to the result
 * value. What we have to do in that case is pick some random readable/writable
 * address in the tracees memory space, and use that for the result instead (we
 * then have to modify the system call to point to that instead). 
 *
 * Template arguments:
 *
 *  Result : The type of the result, e.g., int or siginfo_t.
 *
 *  ZeroTheResult : Whether we should zero out the result memory before passing
 *      it into the wait call.
 *
 *  ResultArgIndex : The index of the argument that points to where the result
 *      should be written.
 */
template <class Result, bool ZeroTheResult, int ResultArgIndex>
class WaitCall : public BlockingCall 
{
private:
    pid_t _waitedId; // same meaning as pid argument of waitpid(2)
    bool _nohang; // does this have WNOHANG?
    Result* _result; // address in tracee's memory space
    unique_ptr<Result> _oldData; // backup of the old data at _result.

protected:
    WaitCall(pid_t target, Result* result, int flags) 
        : _waitedId(target), _nohang(flags & WNOHANG), _result(result) { }

    /* Prepare the wait call, modifying its parameters in the event that the
     * user didn't provide us with a location to store the result.
     *
     * Cleanup:
     *  If false is returned, then reaping the tracee is left to the caller.
     */
    virtual bool prepare(Tracer& tracer, Tracee& tracee);

    /* Retrieve the result and return value of the wait call. Return false if
     * the tracee died and throw an exception if some error occurred. */
    bool get_result(pid_t pid, Result& result, size_t& retval);

    /* Calling these will update the process tree if necessary */
    void on_success(Tracer& tracer, Tracee& tracee, pid_t reaped);
    void on_failure(Tracer& tracer, Tracee& tracee, int error);
};

class Wait4Call : public WaitCall<int, false, 1> 
{
public:
    Wait4Call(pid_t pid, int* status, int flags) 
        : WaitCall<int, false, 1>(pid, status, flags) { }

    virtual bool finalise(Tracer& tracer, Tracee& tracee);
};

/* Converts the arguments used by waitid to the pid argument used by wait4.
 * If the arguments are invalid, then we'll just make something up (it won't
 * end up mattering since the wait call will just return with EINVAL). */
pid_t to_wait4_id(idtype_t type, id_t id) 
{
    switch (type) 
    {
        case P_ALL:
            return -1;
        case P_PID:
            return id;
        case P_PGID:
            return -(pid_t)id;
        default:
            return std::numeric_limits<pid_t>::max(); // clearly wrong
    }
}

class WaitIDCall : public WaitCall<siginfo_t, true, 2> 
{
public:
    WaitIDCall(idtype_t type, id_t id, siginfo_t* infop, int flags) 
        : WaitCall<siginfo_t, true, 2>(to_wait4_id(type, id), infop, flags) { }

    virtual bool finalise(Tracer& tracer, Tracee& t);
};

template <class Result, bool ZeroTheResult, int ResultArgIndex>
bool WaitCall<Result, ZeroTheResult, ResultArgIndex>
::prepare(Tracer& tracer, Tracee& tracee) 
{
    pid_t pid = tracee.pid;
    if (_result == nullptr) 
    {
        // The tracee specified NULL for the address of the result, so find
        // some block of memory in the tracee that we can use to store the
        // syscall's result.
        if (!get_tracee_result_addr(pid, (void*&)_result)) 
        {
            return false;
        }
        // Save the old data that was stored at that address.
        _oldData = std::make_unique<Result>();
        try 
        {
            if (!copy_from_tracee(pid, _oldData.get(), _result, sizeof(Result))) 
            {
                return false;
            }
        } 
        catch (const system_error& e) 
        {
            // intercept EFAULT/EIO, which will occur if the tracee specifies
            // a bad memory address as the result argument for the wait call.
            // If that has happened, then we'll indicate that by setting both
            // _oldData and _result to null. Then when we come to finalise the
            // wait call, we'll just let it fail.
            if (e.code().value() != EFAULT && e.code().value() != EIO) 
            {
                throw e;
            }
            _oldData.reset();
            _result = nullptr;
            return true;
        }
        // Now change the syscall argument to point to this block of memory.
        if (!set_syscall_arg(pid, (size_t)_result, ResultArgIndex)) 
        {
            return false;
        }
    }
    if (ZeroTheResult && !memset_tracee(pid, _result, 0, sizeof(Result))) 
    {
        return false; 
    }
    // Now we notify the process tree that the wait has begun!
    tracee.process->notify_waiting(_waitedId, _nohang);
    return true;
}

template <class Result, bool ZeroTheResult, int ResultArgIndex>
bool WaitCall<Result, ZeroTheResult, ResultArgIndex>
::get_result(pid_t pid, Result& result, size_t& retval) 
{
    // If _result and _oldData are null, then that indicates that the address
    // specified by the tracee for the result is invalid and thus the system
    // call will fail with a memory fault.
    if (_oldData == nullptr && _result == nullptr) 
    {
        retval = -1; // what wait calls return on error
        return true;
    }
    // Retrieve the return value of the syscall
    if (!get_syscall_ret(pid, retval)) 
    {
        return false;
    }
    // Retrieve the result of the wait call
    if (!copy_from_tracee(pid, &result, _result, sizeof(Result))) 
    {
        return false;
    }
    if (_oldData != nullptr) 
    {
        // If _oldData isn't null, then that means we had to pick a random
        // address in the tracee's memory. Let's restore that.
        if (!copy_to_tracee(pid, _result, _oldData.get(), sizeof(Result))) 
        {
            return false;
        }
        // Restore the syscall arg just to be safe. Might not be necessary
        // depending on the syscall calling convention for this architecture.
        if (!set_syscall_arg(pid, 0, ResultArgIndex)) {
            return false;
        }
    }
    return true;
}

template <class Result, bool ZeroTheResult, int ResultArgIndex>
void WaitCall<Result, ZeroTheResult, ResultArgIndex>
::on_success(Tracer& tracer, Tracee& tracee, pid_t chosen)
{
    shared_ptr<Process> child = tracer.find(chosen);
    if (!child)
    {
        throw BadTraceError(tracee.pid, 
            "Tracee reaped a child that I don't know about.");
    }
    tracee.process->notify_reaped(std::move(child));
    Tracee::remove_tracee(tracer, chosen);
}

template <class Result, bool ZeroTheResult, int ResultArgIndex>
void WaitCall<Result, ZeroTheResult, ResultArgIndex>
::on_failure(Tracer& tracer, Tracee& tracee, int error)
{
    tracee.process->notify_failed_wait(error);
}

bool Wait4Call::finalise(Tracer& tracer, Tracee& tracee) 
{
    int status;
    size_t retval;
    if (!get_result(tracee.pid, status, retval)) 
    {
        return false;
    }
    if ((pid_t)retval > 0 && (WIFEXITED(status) || WIFSIGNALED(status))) 
    {
        on_success(tracer, tracee, retval);
    } 
    else if ((pid_t)retval < 0) 
    {
        on_failure(tracer, tracee, -(int)retval);
    }
    return true;
}

bool WaitIDCall::finalise(Tracer& tracer, Tracee& tracee) 
{
    siginfo_t info;
    size_t retval;
    if (!get_result(tracee.pid, info, retval)) 
    {
        return false;
    }
    // waitid will return 0 on success. However, this includes if WNOHANG was
    // specified and nothing happened, so we check info.si_pid to see if the
    // child actually changed state (we zero it out beforehand to be sure).
    if (retval == 0 && info.si_pid != 0
        && (info.si_code == CLD_EXITED
        || info.si_code == CLD_KILLED
        || info.si_code == CLD_DUMPED)) 
    {
        on_success(tracer, tracee, info.si_pid);
    } 
    else if ((int)retval < 0) 
    {
        on_failure(tracer, tracee, -(int)retval);
    }
    return true;
}

/* Call this when we've reached the syscall-exit-stop for a failed fork
 * call. If the fork failed due to the delivery of an interrupting signal,
 * then the failure will be ignored. For any other cause of failure, this 
 * function will exit the program - terminating all tracees (due to the
 * PTRACE_O_EXITKILL option). Leaves reseting tracee.syscall to the caller. */
void Tracer::handle_failed_fork(Tracee& tracee) 
{
    /* We've reached a syscall-exit-stop for the fork call, let's check the
     * return value and determine the cause of failure. */
    size_t retval;
    if (!get_syscall_ret(tracee.pid, retval)) 
    {
        expect_ended(tracee);
        return;
    }
    
    int err = -(long)retval;
    if (err == ERESTARTNOINTR)
    {
        /* The fork call has been interrupted by the delivery of a signal (this
         * error is only visible to ptracers). We'll just return - the tracee
         * will then retry the fork when it next hits syscall-entry-stop, in
         * which case we'll get another go at this. */
        log("{} fork interrupted (to be resumed)", tracee.pid);
        resume(tracee);
        return;
    }

    /* If the fork failed due to any other reason than an interrupting signal,
     * then just kill everything and give up. This is basically a built-in
     * protection against people fork-bombing themselves. (Note that just the
     * act of us exiting will kill all the tracees, since we should have set
     * them up with the PTRACE_O_EXITKILL option). */
    log("{} failed fork: {}", tracee.pid, strerror_s(err));
    log("Nuking everything with SIGKILL and committing suicide :-)");
    _exit(1);
    /* NOTREACHED */
}

/* Also called for fork-like clones. Doesn't work with vfork yet :-( */
void Tracer::handle_fork(Tracee& tracee) 
{
    int status;
    if (!resume(tracee) || !wait_for_stop(tracee, status))
    {
        return;
    }

    if (!IS_FORK_EVENT(status)) 
    {
        if (!IS_SYSCALL_EVENT(status)) 
        {
            throw BadTraceError(tracee.pid, status,
                "Expected syscall-exit-stop after bad fork.");
        }
        tracee.syscall = SYSCALL_NONE;
        handle_failed_fork(tracee);
        return;
    }

    unsigned long childId;
    if (ptrace(PTRACE_GETEVENTMSG, tracee.pid, 0, (void *)&childId) == -1) 
    {
        if (errno == ESRCH) 
        {
            expect_ended(tracee);
            return;
        }
        throw system_error(errno, generic_category(),
            "ptrace(PTRACE_GETEVENTMSG)");
    }

    auto process = std::make_shared<Process>(childId, tracee.process);
    Tracee& child = add_tracee(childId, process);
    child.stopped = false; // The child doesn't start out stopped
    tracee.process->notify_forked(process);

    // Our ptrace config causes SIGSTOP to be raised in the child after fork
    if (!wait_for_stop(child, status))
    {
        return;
    }
    if (WSTOPSIG(status) != SIGSTOP)
    {
        throw BadTraceError(childId, status, "Expected SIGSTOP after fork.");
    }

    // Resume parent until the syscall-exit-stop
    if (!resume(tracee) || !wait_for_stop(tracee, status)) 
    {
        return;
    }
    if (!IS_SYSCALL_EVENT(status)) 
    {
        // TODO what about INTR errors from fork? I guess it already succeeded.
        throw BadTraceError(tracee.pid, status,
            "Expected syscall-exit-stop after fork.");
    }
    tracee.syscall = SYSCALL_NONE;
}

void Tracer::handle_exec(Tracee& tracee, const char* path, const char** argv) 
{
    vector<string> args;
    string file;
    try 
    {
        if (!copy_string_array_from_tracee(tracee.pid, argv, args)
            || !copy_string_from_tracee(tracee.pid, path, file))
        {
            expect_ended(tracee);
            return;
        }
    } 
    catch (const system_error& e) 
    {
        // Intercept EFAULT/EIO - which will occur if the tracee provides bad
        // addresses as arguments to execve. In that cause, just continue on
        // as per usual and let the exec call fail.
        if (e.code().value() != EFAULT && e.code().value() != EIO) 
        {
            throw e;
        }
    }

    int status;
    if (!resume(tracee) || !wait_for_stop(tracee, status)) 
    {
        return;
    }
    if (!IS_EXEC_EVENT(status)) 
    {
        /* Exec has failed!!! */
        if (!IS_SYSCALL_EVENT(status)) 
        {
            throw BadTraceError(tracee.pid, status,
                "Expected a syscall-exit-stop after failed exec.");
        }
        tracee.syscall = SYSCALL_NONE;

        // Get the return value to diagnose the cause of failure.
        size_t retval;
        if (!get_syscall_ret(tracee.pid, retval)) 
        {
            expect_ended(tracee);
            return;
        }

        int err = (long)retval;
        if (err >= 0) 
        {
            log("{}: exec failed but returned {}", tracee.pid, err); // why?
        }

        tracee.process->notify_exec(std::move(file), std::move(args), -err);
        return;
    }

    if (!resume(tracee) || !wait_for_stop(tracee, status)) 
    {
        return;
    }
    if (!IS_SYSCALL_EVENT(status)) 
    {
        throw BadTraceError(tracee.pid, status,
            "Expected syscall-exit-stop after exec.");
    }
    tracee.syscall = SYSCALL_NONE;
    tracee.process->notify_exec(std::move(file), std::move(args), 0);
    if (tracee.pid == _leader)
    {
        _execed = true;
    }
}

void Tracer::initiate_wait(Tracee& tracee, unique_ptr<BlockingCall> wait) 
{
    if (!wait->prepare(*this, tracee)) 
    {
        expect_ended(tracee);
        return;
    }
    tracee.blockingCall = std::move(wait);
}

/* Handle a source location update from tracee using our fake syscall */
void Tracer::handle_new_location(Tracee& tracee,
                                 unsigned line, 
                                 const char* function, 
                                 const char* file) 
{
    SourceLocation location;
    location.line = line;
    if (!copy_string_from_tracee(tracee.pid, function, location.func)) 
    {
        expect_ended(tracee);
        return;
    }
    if (!copy_string_from_tracee(tracee.pid, file, location.file)) 
    {
        expect_ended(tracee);
        return;
    }
    tracee.process->update_location(std::move(location));
    tracee.syscall = SYSCALL_NONE;
    // Since this syscall is not real, there won't be a syscall-exit-stop, so 
    // don't worry about handling that, just keep chugging along.
    resume(tracee);
}

/* Check via the callback if there are currently any orphans available for 
 * us. If we find any, then we'll remove the tracee from our list (this this
 * can invalidate any current iterators into the _tracees map. */
void Tracer::collect_orphans() 
{
    pid_t pid;

next_orphan:
    while (_getOrphanCallback(pid)) 
    {
        auto it = std::find(_recycledPIDs.begin(), _recycledPIDs.end(), pid);
        if (it != _recycledPIDs.end())
        {
            _recycledPIDs.erase(it); // we already removed it
            goto next_orphan;
        }

        auto pair = _tracees.find(pid);
        assert(pair != _tracees.end()); // TODO assert on external factors
        pair->second.process->notify_orphaned();
        _tracees.erase(pair);
        log("{} orphaned", pid);
    }
}

shared_ptr<Process> Tracer::start(string_view program, vector<string> argv) 
{
    if (tracees_alive())
    {
        return nullptr;
    }

    pid_t pid = start_tracee(program, argv); // may throw
    _leader = pid;
    _execed = false;
    auto process = std::make_shared<Process>(pid, program, argv);
    add_tracee(pid, process);

    while (!_execed)
    {
        // Search the map each time since it is possible that the leader has 
        // ended and been removed and old references invalidated.
        auto it = _tracees.find(pid);
        if (it == _tracees.end())
        {
            throw std::runtime_error("Tracee ended before it could exec.");
        }
        if (!resume(it->second))
        {
            expect_ended(it->second);
            throw std::runtime_error("Tracee failed to exec.");
        }
        int status;
        if (waitpid(pid, &status, 0) == -1)
        {
            throw system_error(errno, generic_category(), "waitpid");
        }
        handle_wait_notification(it->second, status);
    }

    return process;
}

shared_ptr<Process> Tracer::find(pid_t pid)
{
    auto it = _tracees.find(pid);
    if (it == _tracees.end())
    {
        return nullptr;
    }
    return it->second.process;
}

bool Tracer::all_tracees_stopped() const
{
    for (auto& pair : _tracees)
    {
        if (!pair.second.stopped)
        {
            return false;
        }
    }
    return true;
}

bool Tracer::wait_for_stop(Tracee& tracee, int& status)
{
    if (waitpid(tracee.pid, &status, 0) == -1) 
    {
        throw system_error(errno, generic_category(), "waitpid");
    }
    if (WIFSTOPPED(status)) 
    {
        tracee.stopped = true;
        return true;
    }
    handle_wait_notification(tracee, status);
    return false;
}

bool Tracer::resume(Tracee& tracee)
{
    if (!tracee.stopped)
    {
        debug("{} not stopped, so not resuming it.", tracee.pid);
        return true; // TODO why would this happen? Should it happen?
    }
    bool ok = resume_tracee(tracee.pid, tracee.signal);
    if (!ok)
    {
        debug("resume_tracee({}) failed", tracee.pid);
    }
    tracee.signal = 0;
    tracee.stopped = false;
    return ok;
}

void Tracer::handle_syscall_entry(Tracee& tracee, 
                                  int syscall,
                                  size_t args[SYS_ARG_MAX])
{
    tracee.syscall = syscall;
    verbose("{} entered syscall {}", tracee.pid, get_syscall_name(syscall));
    switch (syscall) 
    {
        case SYSCALL_PTRACE:
        case SYSCALL_SETPGID:
        case SYSCALL_SETSID:
        case SYSCALL_VFORK:
            break; // we'll block these syscalls

        case SYSCALL_FORK:
            handle_fork(tracee);
            return;

        case SYSCALL_EXECVE:
            handle_exec(tracee, (const char*)args[0], (const char**)args[1]);
            return;

        case SYSCALL_EXECVEAT:
            handle_exec(tracee, (const char*)args[1], (const char**)args[2]);
            return;

        case SYSCALL_WAIT4:
            initiate_wait(tracee, std::make_unique<Wait4Call>(
                (pid_t)args[0],
                (int*)args[1],
                (int)args[2]
            ));
            return;

        case SYSCALL_WAITID:
            initiate_wait(tracee, std::make_unique<WaitIDCall>(
                (idtype_t)args[0],
                (id_t)args[1],
                (siginfo_t*)args[2],
                (int)args[3]
            ));
            return;

        case SYSCALL_CLONE:
            // TODO what if CLONE_THREAD or CLONE_PARENT is specified???
            // Then that is very much *unlike* a fork...?!?!?!
            if (IS_CLONE_LIKE_A_FORK(args)) 
            {
                handle_fork(tracee);
                return;
            } 
            break; // we'll cancel the syscall

        case SYSCALL_FAKE:
            handle_new_location(
                tracee,
                (unsigned)args[0],
                (const char*)args[1],
                (const char*)args[2]
            );
            return;

        case SYSCALL_KILL: // TODO implement these
        case SYSCALL_TKILL:
        case SYSCALL_TGKILL:
        default:
            resume(tracee);
            return;
    }

    error("Tracee {} tried to execute banned syscall {}.", 
        tracee.pid, get_syscall_name(syscall));
    set_syscall(tracee.pid, SYSCALL_NONE); // make the syscall fail
    resume(tracee);
}

void Tracer::handle_syscall_exit(Tracee& tracee)
{
    if (tracee.blockingCall != nullptr) 
    {
        // we just reached the syscall-exit-stop for a blocking system
        // call that we were trying to keep track of - so finish that.
        if (!tracee.blockingCall->finalise(*this, tracee)) 
        {
            expect_ended(tracee);
            return;
        }
        verbose("{} exited blocking syscall {}", 
            tracee.pid, get_syscall_name(tracee.syscall));
        tracee.blockingCall.reset();
    }
    else
    {
        verbose("{} exited syscall {}", 
            tracee.pid, get_syscall_name(tracee.syscall));
        resume(tracee);
    }
    tracee.syscall = SYSCALL_NONE;
}

void Tracer::handle_signal_stop(Tracee& tracee, int signal)
{
    if (tracee.signal != 0)
    {
        // TODO I don't know if this is allowed by Linux/ptrace or not
        // I may have to be able to support a list of pending signals...
        // I don't think this is a problem since Linux only delivers the
        // signals when the process resumes. Will think more about this.
        throw BadTraceError(tracee.pid, "Tracee delivered a signal when there"
            " was already a pending signal.");
    }

    siginfo_t info;
    if (ptrace(PTRACE_GETSIGINFO, tracee.pid, 0, &info) == -1)
    {
        if (errno == ESRCH)
        {
            expect_ended(tracee);
            return;
        }
        throw system_error(errno, generic_category(), 
            "ptrace(PTRACE_GETSIGINFO)");
    }

    tracee.process->notify_signaled(info.si_pid, signal);
    tracee.signal = signal; // make sure it's delivered when next resumed
}

/* Calls waitpid on the tracee and makes sure that it's either exited or been
 * killed, then handles the exit/kill event with handle_wait_notification. 
 * If the tracee hasn't ended, then a BadTraceError is thrown. */
void Tracer::expect_ended(Tracee& tracee)
{
    // TODO maybe use WNOHANG? Maybe loop until we get an exit event in case
    // multiple were queued up (is that even possible?). Grrr so much of this
    // depends on me know the precise behaviour of Linux and sometimes the only
    // solution is to either test it or read the source code.
    int status;
    if (waitpid(tracee.pid, &status, 0) == -1)
    {
        if (errno == ECHILD)
        {
            throw BadTraceError(tracee.pid, 
                "Expected tracee to have ended but it doesn't exist.");
        }
        throw system_error(errno, generic_category(), "waitpid");
    }
    if (!WIFEXITED(status) && !WIFSIGNALED(status))
    {
        throw BadTraceError(tracee.pid, status,
            "Expected tracee to have ended, but it hasn't.");
    }

    handle_wait_notification(tracee, status);
}

void Tracer::handle_wait_notification(Tracee& tracee, int status)
{
    if (WIFEXITED(status) || WIFSIGNALED(status))
    {
        tracee.process->notify_ended(status);
        _tracees.erase(tracee.pid);
        return;
    }

    if (!WIFSTOPPED(status))
    {
        throw BadTraceError(tracee.pid, status,
            "Tracee hasn't ended but also hasn't stopped...");
    }
    tracee.stopped = true;

    if (IS_SYSCALL_EVENT(status))
    {
        if (tracee.syscall == SYSCALL_NONE)
        {
            int syscall;
            size_t args[SYS_ARG_MAX];
            if (!which_syscall(tracee.pid, syscall, args))
            {
                expect_ended(tracee);
                return;
            }
            handle_syscall_entry(tracee, syscall, args);
        }
        else
        {
            handle_syscall_exit(tracee);
        }
    }
    else if (IS_FORK_EVENT(status) 
        || IS_CLONE_EVENT(status) 
        || IS_EXEC_EVENT(status)
        || IS_EXIT_EVENT(status))
    {
        // These events should only be generated when handling the respective
        // system calls. They should not be appearing now.
        throw BadTraceError(tracee.pid, status, "Got an event at weird time.");
    }
    else
    {
        handle_signal_stop(tracee, WSTOPSIG(status));
    }

}

Tracee& Tracer::add_tracee(pid_t pid, shared_ptr<Process> process)
{
    auto it = _tracees.find(pid);
    if (it != _tracees.end())
    {
        // We got a new tracee with the same PID as an existing tracee. This is
        // possible if the old tracee was orphaned and the reaper reaped it,
        // but the system recycled the PID before we learnt about it. This is
        // extremely unlikely to occur but why not be prepared for it.
        _tracees.erase(it); // it ded
        _recycledPIDs.push_back(pid);
    }
    return (_tracees[pid] = Tracee(pid, std::move(process)));
}

bool Tracer::step() 
{
    for (auto& [pid, tracee] : _tracees) 
    {
        resume_tracee(pid, tracee.signal); // Ignore ESRCH failure TODO
        tracee.signal = 0;
    }
    collect_orphans();

    int status;
    pid_t pid;
    while ((pid = wait(&status)) != -1) 
    {
        auto it = _tracees.find(pid);
        if (it == _tracees.end())
        {
            warning("Got wait status {} for unknown PID {}.", status, pid);
            continue;
        }

        handle_wait_notification(it->second, status);
        collect_orphans(); // this could invalidate the iterator!!!

        if (all_tracees_stopped())
        {
            return true;
        }
    }
    while (!_tracees.empty()) 
    {
        collect_orphans();
    }
    return false;
}

void Tracer::nuke() 
{
    if (_leader == -1 || _tracees.empty())
    {
        return;
    }
    if (killpg(_leader, SIGKILL) == -1)
    {
        throw system_error(errno, generic_category(), "killpg");
    }
    //while (step()) { }
    //assert(_tracees.empty());
    //assert(_recycledPIDs.empty()); // TODO ??
}

void Tracer::print_list() const 
{
    for (auto& [pid, tracee] : _tracees) 
    {
        std::cerr << format("{} {} {}\n", 
            pid, tracee.process->state(), tracee.process->command_line());
    }
    std::cerr << "total: " << _tracees.size() << '\n';
}
