/* -*- Mode: C++; tab-width: 8; c-basic-offset: 2; indent-tabs-mode: nil; -*- */

//#define DEBUGTAG "ReplayTimeline"

#include "ReplayTimeline.h"

#include <math.h>

#include "fast_forward.h"
#include "log.h"

using namespace rr;
using namespace std;

ReplayTimeline::InternalMark::~InternalMark() {
  if (owner && checkpoint) {
    owner->remove_mark_with_checkpoint(key);
  }
}

ostream& operator<<(ostream& s, const ReplayTimeline::MarkKey& o) {
  return s << "time:" << o.trace_time << ",ticks:" << o.ticks
           << ",st:" << o.step_key.as_int();
}

ostream& operator<<(ostream& s, const ReplayTimeline::InternalMark& o) {
  return s << "{" << o.key << ",regs_ip:" << o.regs.ip() << "}";
}

ostream& operator<<(ostream& s, const ReplayTimeline::Mark& o) {
  if (!o.ptr) {
    return s << "{null}";
  }
  return s << *o.ptr.get();
}

ostream& operator<<(ostream& s, const ReplayTimeline::ProtoMark& o) {
  return s << "{" << o.key << ",regs_ip:" << o.regs.ip() << "}";
}

bool ReplayTimeline::less_than(const Mark& m1, const Mark& m2) {
  assert(m1.ptr->owner == m2.ptr->owner);
  if (m1.ptr->key < m2.ptr->key) {
    return true;
  }
  if (m2.ptr->key < m1.ptr->key) {
    return false;
  }
  if (!m1.ptr->owner) {
    return false;
  }
  for (shared_ptr<InternalMark>& m : m1.ptr->owner->marks[m1.ptr->key]) {
    if (m == m2.ptr) {
      return false;
    }
    if (m == m1.ptr) {
      return true;
    }
  }
  assert(0 && "Marks missing from vector, invariants broken!");
  return false;
}

ReplayTimeline::ReplayTimeline(std::shared_ptr<ReplaySession> session,
                               const ReplaySession::Flags& session_flags)
    : session_flags(session_flags),
      current(std::move(session)),
      breakpoints_applied(false),
      reverse_execution_barrier_event(0) {
  current->set_visible_execution(false);
  current->set_flags(session_flags);
}

ReplayTimeline::~ReplayTimeline() {
  for (auto it : marks) {
    for (shared_ptr<InternalMark>& itv : it.second) {
      itv->owner = nullptr;
      itv->checkpoint = nullptr;
    }
  }
}

static bool equal_regs(const Registers& r1, const Registers& r2) {
  // Compare ip()s first since they will usually fail to match, especially
  // when we're comparing InternalMarks with the same MarkKey
  return r1.ip() == r2.ip() && r1.matches(r2);
}

bool ReplayTimeline::InternalMark::equal_states(Task* t) const {
  return equal_regs(regs, t->regs()) &&
         return_addresses == t->return_addresses();
}

bool ReplayTimeline::ProtoMark::equal_states(Task* t) const {
  return equal_regs(regs, t->regs()) &&
         return_addresses == t->return_addresses();
}

ReplayTimeline::ProtoMark ReplayTimeline::proto_mark() const {
  return ProtoMark(current_mark_key(), current->current_task());
}

shared_ptr<ReplayTimeline::InternalMark> ReplayTimeline::current_mark() {
  Task* t = current->current_task();
  auto it = marks.find(current_mark_key());
  // Avoid creating an entry in 'marks' if it doesn't already exist
  if (it != marks.end()) {
    for (shared_ptr<InternalMark>& m : it->second) {
      if (m->equal_states(t)) {
        return m;
      }
    }
  }
  return shared_ptr<InternalMark>();
}

ReplayTimeline::Mark ReplayTimeline::mark() {
  Mark result;
  auto cm = current_mark();
  if (cm) {
    swap(cm, result.ptr);
    return result;
  }

  MarkKey key = current_mark_key();
  shared_ptr<InternalMark> m = make_shared<InternalMark>(this, *current, key);

  auto& mark_vector = marks[key];
  if (mark_vector.empty()) {
    mark_vector.push_back(m);
  } else if (mark_vector[mark_vector.size() - 1] == current_at_or_after_mark) {
    mark_vector.push_back(m);
  } else {
    // Now the hard part: figuring out where to put it in the list of existing
    // marks.
    unapply_breakpoints_and_watchpoints();
    ReplaySession::shr_ptr tmp_session = current->clone();
    vector<shared_ptr<InternalMark> >::iterator mark_index = mark_vector.end();

    // We could set breakpoints at the marks and then continue with an
    // interrupt set to fire when our tick-count increases. But that requires
    // new replay functionality (probably a new RunCommand), so for now, do the
    // simplest thing and just single-step until we find where to put the new
    // mark(s).
    vector<shared_ptr<InternalMark> > new_marks;
    new_marks.push_back(m);

    LOG(debug) << "mark() replaying to find mark location";

    // Allow coalescing of multiple repetitions of a single x86 string
    // instruction (as long as we don't reach one of our mark_vector states).
    ReplaySession::StepConstraints constraints(RUN_SINGLESTEP_FAST_FORWARD);
    for (auto& mv : mark_vector) {
      constraints.stop_before_states.push_back(&mv->regs);
    }

    while (true) {
      auto result = tmp_session->replay_step(constraints);
      if (session_mark_key(*tmp_session) != key ||
          result.status != REPLAY_CONTINUE) {
        break;
      }
      if (!result.break_status.singlestep_complete) {
        continue;
      }

      Task* t = tmp_session->current_task();
      for (auto it = mark_vector.begin(); it != mark_vector.end(); ++it) {
        shared_ptr<InternalMark>& existing_mark = *it;
        if (existing_mark->equal_states(t)) {
          if (!result.did_fast_forward && !result.break_status.signal) {
            new_marks.back()->singlestep_to_next_mark_no_signal = true;
          }
          mark_index = it;
          break;
        }
      }
      if (mark_index != mark_vector.end()) {
        break;
      }

      // Some callers singlestep through N instructions, all with the same
      // MarkKey, requesting a Mark after each step. If there's a Mark at the
      // end of the N instructions, this could mean N(N+1)/2 singlestep
      // operations total. To avoid that, add all the intermediate states to
      // the mark map now, so the first mark() call will perform N singlesteps
      // and the rest will perform none.
      if (!result.did_fast_forward && !result.break_status.signal) {
        new_marks.back()->singlestep_to_next_mark_no_signal = true;
      }
      new_marks.push_back(make_shared<InternalMark>(this, *tmp_session, key));
    }

    LOG(debug) << "Mark location found";

    // mark_index is the current index of the next mark after 'current'. So
    // insert our new marks at mark_index.
    mark_vector.insert(mark_index, new_marks.begin(), new_marks.end());
  }
  swap(m, result.ptr);
  current_at_or_after_mark = result.ptr;
  return result;
}

void ReplayTimeline::mark_after_singlestep(const Mark& from,
                                           const ReplayResult& result) {
  Mark m = mark();
  if (!result.did_fast_forward && m.ptr->key == from.ptr->key &&
      !result.break_status.signal) {
    auto& mark_vector = marks[m.ptr->key];
    for (size_t i = 0; i < mark_vector.size(); ++i) {
      if (mark_vector[i] == from.ptr) {
        assert(i + 1 < mark_vector.size() && mark_vector[i + 1] == m.ptr);
        break;
      }
    }
    from.ptr->singlestep_to_next_mark_no_signal = true;
  }
}

ReplayTimeline::Mark ReplayTimeline::find_singlestep_before(const Mark& mark) {
  auto& mark_vector = marks[mark.ptr->key];
  ssize_t i;
  for (i = mark_vector.size() - 1; i >= 0; --i) {
    if (mark_vector[i] == mark.ptr) {
      break;
    }
  }
  assert(i >= 0 && "Mark not in vector???");

  Mark m;
  if (i == 0) {
    return m;
  }
  if (!mark_vector[i - 1]->singlestep_to_next_mark_no_signal) {
    return m;
  }
  m.ptr = mark_vector[i - 1];
  return m;
}

ReplayTimeline::Mark ReplayTimeline::lazy_reverse_singlestep(const Mark& from,
                                                             Task* t) {
  if (!no_watchpoints_hit_interval_start || !no_watchpoints_hit_interval_end) {
    return Mark();
  }
  Mark m = find_singlestep_before(from);
  if (m && m >= no_watchpoints_hit_interval_start &&
      m < no_watchpoints_hit_interval_end &&
      !has_breakpoint_at_address(t, from.ptr->regs.ip())) {
    return m;
  }
  return Mark();
}

ReplayTimeline::Mark ReplayTimeline::add_explicit_checkpoint() {
  assert(current->can_clone());

  Mark m = mark();
  if (!m.ptr->checkpoint) {
    unapply_breakpoints_and_watchpoints();
    m.ptr->checkpoint = current->clone();
    auto key = m.ptr->key;
    if (marks_with_checkpoints.find(key) == marks_with_checkpoints.end()) {
      marks_with_checkpoints[key] = 1;
    } else {
      marks_with_checkpoints[key]++;
    }
  }
  ++m.ptr->checkpoint_refcount;
  return m;
}

void ReplayTimeline::remove_mark_with_checkpoint(const MarkKey& key) {
  assert(marks_with_checkpoints[key] > 0);
  if (--marks_with_checkpoints[key] == 0) {
    marks_with_checkpoints.erase(key);
  }
}

void ReplayTimeline::remove_explicit_checkpoint(const Mark& mark) {
  assert(mark.ptr->checkpoint_refcount > 0);
  if (--mark.ptr->checkpoint_refcount == 0) {
    mark.ptr->checkpoint = nullptr;
    remove_mark_with_checkpoint(mark.ptr->key);
  }
}

void ReplayTimeline::seek_to_before_key(const MarkKey& key) {
  auto it = marks_with_checkpoints.lower_bound(key);
  // 'it' points to the first value equivalent to or greater than 'key'.
  auto current_key = current_mark_key();
  if (it == marks_with_checkpoints.begin()) {
    if (current_key < key) {
      // We can use the current session, so do nothing.
    } else {
      // nowhere earlier to go, so restart from beginning.
      current = ReplaySession::create(current->trace_reader().dir());
      breakpoints_applied = false;
      current_at_or_after_mark = nullptr;
      current->set_flags(session_flags);
    }
  } else {
    --it;
    // 'it' is now at the last checkpoint before 'key'
    if (it->first < current_key && current_key < key) {
      // Current state is closer to the destination than any checkpoint we
      // have, so do nothing.
    } else {
      // Return one of the checkpoints at *it.
      current = nullptr;
      for (auto mark_it : marks[it->first]) {
        shared_ptr<InternalMark> m(mark_it);
        if (m->checkpoint) {
          current = m->checkpoint->clone();
          // At this point, m->checkpoint is fully initialized but current
          // is not. Swap them so that m->checkpoint is not fully
          // initialized, to reduce resource usage.
          swap(current, m->checkpoint);
          break;
        }
      }
      assert(current);
      breakpoints_applied = false;
      current_at_or_after_mark = nullptr;
    }
  }
}

void ReplayTimeline::seek_up_to_mark(const Mark& mark) {
  if (current_mark_key() == mark.ptr->key) {
    Mark cm = this->mark();
    if (cm <= mark) {
      // close enough, stay where we are
      return;
    }
  }

  // Check if any of the marks with the same key as 'mark', but not after
  // 'mark', are usable.
  auto& mark_vector = marks[mark.ptr->key];
  bool at_or_before_mark = false;
  for (ssize_t i = mark_vector.size() - 1; i >= 0; --i) {
    auto& m = mark_vector[i];
    if (m == mark.ptr) {
      at_or_before_mark = true;
    }
    if (at_or_before_mark && m->checkpoint) {
      current = m->checkpoint->clone();
      // At this point, m->checkpoint is fully initialized but current
      // is not. Swap them so that m->checkpoint is not fully
      // initialized, to reduce resource usage.
      swap(current, m->checkpoint);
      breakpoints_applied = false;
      current_at_or_after_mark = m;
      return;
    }
  }

  return seek_to_before_key(mark.ptr->key);
}

ReplaySession::StepConstraints
ReplayTimeline::ReplayStepToMarkStrategy::setup_step_constraints() {
  ReplaySession::StepConstraints constraints(RUN_CONTINUE);
  if (singlesteps_to_perform > 0) {
    constraints.command = RUN_SINGLESTEP_FAST_FORWARD;
    --singlesteps_to_perform;
  }
  return constraints;
}

void ReplayTimeline::update_strategy_and_fix_watchpoint_quirk(
    ReplayStepToMarkStrategy& strategy,
    const ReplaySession::StepConstraints& constraints, ReplayResult& result,
    const ProtoMark& before) {
  if (constraints.command == RUN_CONTINUE &&
      fix_watchpoint_coalescing_quirk(result, before)) {
    // It's quite common for x86 string instructions to trigger the same
    // watchpoint several times in consecutive instructions, e.g. if we're
    // doing a "rep movsb" over an 8-byte watchpoint. 8 invocations of
    // fix_watchpoint_coalescing_quirk could require 8 replays from some
    // previous checkpoint. To avoid that, after
    // fix_watchpoint_coalescing_quirk has fired once, singlestep the
    // next 7 times.
    strategy.singlesteps_to_perform = 7;
  }
}

ReplayResult ReplayTimeline::replay_step_to_mark(
    const Mark& mark, ReplayStepToMarkStrategy& strategy) {
  ProtoMark before = proto_mark();
  ReplayResult result;
  if (current->trace_reader().time() < mark.ptr->key.trace_time) {
    // Easy case: each RUN_CONTINUE can only advance by at most one
    // trace event, so do one. But do a singlestep if our strategy suggests
    // we should.
    ReplaySession::StepConstraints constraints =
        strategy.setup_step_constraints();
    constraints.stop_at_time = mark.ptr->key.trace_time;
    result = current->replay_step(constraints);
    update_strategy_and_fix_watchpoint_quirk(strategy, constraints, result,
                                             before);
    return result;
  }

  Task* t = current->current_task();
  ASSERT(t, current->trace_reader().time() == mark.ptr->key.trace_time);
  // t must remain valid through here since t can only die when we complete
  // an event, and we're not going to complete another event before
  // reaching the mark ... apart from where we call
  // fix_watchpoint_coalescing_quirk.

  if (t->tick_count() < mark.ptr->key.ticks) {
    // Try to make progress by just continuing with a ticks constraint
    // set to stop us before the mark. This is efficient in the worst case,
    // when we must execute lots of instructions to reach the mark.
    ReplaySession::StepConstraints constraints =
        strategy.setup_step_constraints();
    constraints.ticks_target = mark.ptr->key.ticks - 1;
    result = current->replay_step(constraints);
    bool approaching_ticks_target =
        result.break_status.approaching_ticks_target;
    result.break_status.approaching_ticks_target = false;
    // We can't be at the mark yet.
    ASSERT(t, t->tick_count() < mark.ptr->key.ticks);
    // If there's a break indicated, we should return that to the
    // caller without doing any more work
    if (!approaching_ticks_target || result.break_status.any_break()) {
      update_strategy_and_fix_watchpoint_quirk(strategy, constraints, result,
                                               before);
      return result;
    }
    // We may not have made any progress so we'll need to try another strategy
  }

  remote_code_ptr mark_addr = mark.ptr->regs.ip();

  // Try adding a breakpoint at the required IP and running to it.
  // We can't do this if we're currently at the IP, since we'd make no progress.
  // Setting the breakpoint may fail; the mark address may be in invalid
  // memory, e.g. because it's at the delivery of a SIGSEGV for a bad IP.
  if (t->regs().ip() != mark_addr &&
      t->vm()->add_breakpoint(mark_addr, TRAP_BKPT_USER)) {
    ReplaySession::StepConstraints constraints =
        strategy.setup_step_constraints();
    result = current->replay_step(constraints);
    t->vm()->remove_breakpoint(mark_addr, TRAP_BKPT_USER);
    // If we hit our breakpoint and there is no client breakpoint there,
    // pretend we didn't hit it.
    if (result.break_status.breakpoint_hit &&
        !has_breakpoint_at_address(result.break_status.task,
                                   result.break_status.task->ip())) {
      result.break_status.breakpoint_hit = false;
    }
    update_strategy_and_fix_watchpoint_quirk(strategy, constraints, result,
                                             before);
    return result;
  }

  // At required IP, but not in the correct state. Singlestep over this IP.
  // We need the FAST_FORWARD option in case the mark state occurs after
  // many iterations of a string instruction at this address.
  ReplaySession::StepConstraints constraints(RUN_SINGLESTEP_FAST_FORWARD);
  // We don't want to fast-forward past the mark state, so give the mark
  // state as a state we should stop before. FAST_FORWARD always does at
  // least one singlestep so one call to replay_step_to_mark will fast-forward
  // to the state before the mark and return, then the next call to
  // replay_step_to_mark will singlestep into the mark state.
  constraints.stop_before_states.push_back(&mark.ptr->regs);
  result = current->replay_step(constraints);
  // Hide internal singlestep but preserve other break statuses
  result.break_status.singlestep_complete = false;
  return result;
}

void ReplayTimeline::seek_to_proto_mark(const ProtoMark& pmark) {
  seek_to_before_key(pmark.key);
  unapply_breakpoints_and_watchpoints();
  while (current_mark_key() != pmark.key ||
         !pmark.equal_states(current->current_task())) {
    if (current->trace_reader().time() < pmark.key.trace_time) {
      ReplaySession::StepConstraints constraints(RUN_CONTINUE);
      constraints.stop_at_time = pmark.key.trace_time;
      current->replay_step(constraints);
    } else {
      Task* t = current->current_task();
      remote_code_ptr mark_addr = pmark.regs.ip();
      if (t->regs().ip() == mark_addr &&
          current->current_step_key().in_execution()) {
        // At required IP, but not in the correct state. Singlestep over
        // this IP.
        ReplaySession::StepConstraints constraints(RUN_SINGLESTEP_FAST_FORWARD);
        constraints.stop_before_states.push_back(&pmark.regs);
        current->replay_step(constraints);
      } else {
        // Get a shared reference to t->vm() in case t dies during replay_step
        shared_ptr<AddressSpace> vm = t->vm();
        vm->add_breakpoint(mark_addr, TRAP_BKPT_USER);
        current->replay_step(RUN_CONTINUE);
        vm->remove_breakpoint(mark_addr, TRAP_BKPT_USER);
      }
    }
  }
}

void ReplayTimeline::seek_to_mark(const Mark& mark) {
  seek_up_to_mark(mark);
  while (current_mark() != mark.ptr) {
    unapply_breakpoints_and_watchpoints();
    ReplayStepToMarkStrategy strategy;
    replay_step_to_mark(mark, strategy);
  }
  current_at_or_after_mark = mark.ptr;
  // XXX handle cases where breakpoints can't yet be applied
}

/**
 * Intel CPUs (and maybe others) coalesce iterations of REP-prefixed string
 * instructions so that a watchpoint on a byte at location L can fire after
 * the iteration that writes byte L+63 (or possibly more?).
 * This causes problems for rr since this coalescing doesn't happen when we
 * single-step.
 * This function is called after doing a ReplaySession::replay_step with
 * command == RUN_CONTINUE. RUN_SINGLESTEP and RUN_SINGLESTEP_FAST_FORWARD
 * disable this coalescing (the latter, because it's aware of watchpoints
 * and single-steps when it gets too close to them).
 * |before| is the state before we did the replay_step.
 * If a watchpoint fired, and it looks like it could have fired during a
 * string instruction, we'll backup to |before| and replay forward, stopping
 * before the breakpoint could fire and single-stepping to make sure the
 * coalescing quirk doesn't happen.
 * Returns true if we might have fixed something.
 */
bool ReplayTimeline::fix_watchpoint_coalescing_quirk(ReplayResult& result,
                                                     const ProtoMark& before) {
  if (result.status == REPLAY_EXITED ||
      result.break_status.watchpoints_hit.empty()) {
    // no watchpoint hit. Nothing to fix.
    return false;
  }
  if (!maybe_at_or_after_x86_string_instruction(result.break_status.task)) {
    return false;
  }

  TaskUid after_tuid = result.break_status.task->tuid();
  Ticks after_ticks = result.break_status.task->tick_count();
  LOG(debug) << "Fixing x86-string coalescing quirk from " << before << " to "
             << proto_mark() << " (final cx "
             << result.break_status.task->regs().cx() << ")";

  seek_to_proto_mark(before);

  // Keep going until the watchpoint fires. It will either fire early, or at
  // the same time as some other break.
  apply_breakpoints_and_watchpoints();
  bool approaching_ticks_target = false;
  while (true) {
    Task* t = current->current_task();
    if (t->tuid() == after_tuid) {
      if (approaching_ticks_target) {
        // We don't need to set any stop_before_states here.
        // RUN_SINGLESTEP_FAST_FORWARD always avoids the coalescing quirk, so
        // if a watchpoint is triggered by the string instruction at
        // string_instruction_ip, it will have the correct timing.
        result = current->replay_step(RUN_SINGLESTEP_FAST_FORWARD);
        if (!result.break_status.watchpoints_hit.empty()) {
          LOG(debug) << "Fixed x86-string coalescing quirk; now at "
                     << current_mark_key() << " (new cx "
                     << result.break_status.task->regs().cx() << ")";
          break;
        }
      } else {
        ReplaySession::StepConstraints constraints(RUN_CONTINUE);
        constraints.ticks_target = after_ticks - 1;
        result = current->replay_step(constraints);
        approaching_ticks_target = result.break_status.approaching_ticks_target;
      }
      ASSERT(t, t->tick_count() <= after_ticks) << "We went too far!";
    } else {
      current->replay_step(RUN_CONTINUE);
    }
  }
  return true;
}

bool ReplayTimeline::add_breakpoint(
    Task* t, remote_code_ptr addr,
    std::unique_ptr<BreakpointCondition> condition) {
  if (has_breakpoint_at_address(t, addr)) {
    remove_breakpoint(t, addr);
  }
  // Apply breakpoints now; we need to actually try adding this breakpoint
  // to see if it works.
  apply_breakpoints_and_watchpoints();
  if (!t->vm()->add_breakpoint(addr, TRAP_BKPT_USER)) {
    return false;
  }
  breakpoints.insert(make_tuple(t->vm()->uid(), addr, move(condition)));
  return true;
}

void ReplayTimeline::remove_breakpoint(Task* t, remote_code_ptr addr) {
  if (breakpoints_applied) {
    t->vm()->remove_breakpoint(addr, TRAP_BKPT_USER);
  }
  ASSERT(t, has_breakpoint_at_address(t, addr));
  auto it = breakpoints.lower_bound(make_tuple(t->vm()->uid(), addr, nullptr));
  breakpoints.erase(it);
}

bool ReplayTimeline::has_breakpoint_at_address(Task* t, remote_code_ptr addr) {
  auto it = breakpoints.lower_bound(make_tuple(t->vm()->uid(), addr, nullptr));
  return it != breakpoints.end() && get<0>(*it) == t->vm()->uid() &&
         get<1>(*it) == addr;
}

bool ReplayTimeline::add_watchpoint(Task* t, remote_ptr<void> addr,
                                    size_t num_bytes, WatchType type,
                                    unique_ptr<BreakpointCondition> condition) {
  if (has_watchpoint_at_address(t, addr, num_bytes, type)) {
    remove_watchpoint(t, addr, num_bytes, type);
  }
  // Apply breakpoints now; we need to actually try adding this breakpoint
  // to see if it works.
  apply_breakpoints_and_watchpoints();
  if (!t->vm()->add_watchpoint(addr, num_bytes, type)) {
    return false;
  }
  watchpoints.insert(
      make_tuple(t->vm()->uid(), addr, num_bytes, type, move(condition)));
  no_watchpoints_hit_interval_start = no_watchpoints_hit_interval_start =
      Mark();
  return true;
}

void ReplayTimeline::remove_watchpoint(Task* t, remote_ptr<void> addr,
                                       size_t num_bytes, WatchType type) {
  if (breakpoints_applied) {
    t->vm()->remove_watchpoint(addr, num_bytes, type);
  }
  ASSERT(t, has_watchpoint_at_address(t, addr, num_bytes, type));
  auto it = watchpoints.lower_bound(
      make_tuple(t->vm()->uid(), addr, num_bytes, type, nullptr));
  watchpoints.erase(it);
}

bool ReplayTimeline::has_watchpoint_at_address(Task* t, remote_ptr<void> addr,
                                               size_t num_bytes,
                                               WatchType type) {
  auto it = watchpoints.lower_bound(
      make_tuple(t->vm()->uid(), addr, num_bytes, type, nullptr));
  return it != watchpoints.end() && get<0>(*it) == t->vm()->uid() &&
         get<1>(*it) == addr && get<2>(*it) == num_bytes && get<3>(*it) == type;
}

void ReplayTimeline::remove_breakpoints_and_watchpoints() {
  unapply_breakpoints_and_watchpoints();
  breakpoints.clear();
  watchpoints.clear();
}

void ReplayTimeline::apply_breakpoints_and_watchpoints() {
  if (breakpoints_applied) {
    return;
  }
  breakpoints_applied = true;
  for (auto& bp : breakpoints) {
    AddressSpace* vm = current->find_address_space(get<0>(bp));
    // XXX handle cases where we can't apply a breakpoint right now. Later
    // during replay the address space might be created (or new mappings might
    // be created) and we should reapply breakpoints then.
    if (vm) {
      vm->add_breakpoint(get<1>(bp), TRAP_BKPT_USER);
    }
  }
  for (auto& wp : watchpoints) {
    AddressSpace* vm = current->find_address_space(get<0>(wp));
    // XXX handle cases where we can't apply a watchpoint right now. Later
    // during replay the address space might be created (or new mappings might
    // be created) and we should reapply watchpoints then.
    // XXX we could make this more efficient by providing a method to set
    // several watchpoints at once on a given AddressSpace.
    if (vm) {
      vm->add_watchpoint(get<1>(wp), get<2>(wp), get<3>(wp));
    }
  }
}

void ReplayTimeline::unapply_breakpoints_and_watchpoints() {
  if (!breakpoints_applied) {
    return;
  }
  breakpoints_applied = false;
  for (auto& vm : current->vms()) {
    vm->remove_all_breakpoints();
    vm->remove_all_watchpoints();
  }
}

ReplayResult ReplayTimeline::singlestep_with_breakpoints_disabled() {
  apply_breakpoints_and_watchpoints();
  for (auto& vm : current->vms()) {
    vm->remove_all_breakpoints();
  }
  auto result = current->replay_step(RUN_SINGLESTEP);
  for (auto& bp : breakpoints) {
    AddressSpace* vm = current->find_address_space(get<0>(bp));
    if (vm) {
      vm->add_breakpoint(get<1>(bp), TRAP_BKPT_USER);
    }
  }
  return result;
}

bool ReplayTimeline::is_start_of_reverse_execution_barrier_event() {
  if (current->trace_reader().time() != reverse_execution_barrier_event ||
      current->current_step_key().in_execution()) {
    return false;
  }
  LOG(debug) << "Found reverse execution barrier at " << mark();
  return true;
}

bool ReplayTimeline::run_forward_to_intermediate_point(const Mark& end,
                                                       ForceProgress force) {
  unapply_breakpoints_and_watchpoints();

  LOG(debug) << "Trying to find intermediate point between "
             << current_mark_key() << " and " << end
             << (force == FORCE_PROGRESS ? " (forced)" : "");

  TraceFrame::Time now = current->trace_reader().time();
  TraceFrame::Time mid = (now + end.ptr->key.trace_time) / 2;
  if (now < mid && mid < end.ptr->key.trace_time) {
    ReplaySession::StepConstraints constraints(RUN_CONTINUE);
    constraints.stop_at_time = mid;
    while (current->trace_reader().time() < mid) {
      current->replay_step(constraints);
    }
    assert(current->trace_reader().time() == mid);
    LOG(debug) << "Ran forward to mid event " << current_mark_key();
    return true;
  }

  if (current->trace_reader().time() < end.ptr->key.trace_time &&
      end.ptr->ticks_at_event_start < end.ptr->key.ticks) {
    ReplaySession::StepConstraints constraints(RUN_CONTINUE);
    constraints.stop_at_time = end.ptr->key.trace_time;
    while (current->trace_reader().time() < end.ptr->key.trace_time) {
      current->replay_step(constraints);
    }
    assert(current->trace_reader().time() == end.ptr->key.trace_time);
    LOG(debug) << "Ran forward to event " << current_mark_key();
    return true;
  }

  Task* t = current->current_task();
  if (t) {
    Ticks start_ticks = t->tick_count();
    Ticks end_ticks = current->current_trace_frame().ticks();
    if (end.ptr->key.trace_time == current->trace_reader().time()) {
      end_ticks = min(end_ticks, end.ptr->key.ticks);
    }
    ASSERT(t, start_ticks <= end_ticks);
    Ticks target = min(end_ticks, (start_ticks + end_ticks) / 2);
    ReplaySession::StepConstraints constraints(RUN_CONTINUE);
    constraints.ticks_target = target;
    ProtoMark m = proto_mark();
    ReplayResult result = current->replay_step(constraints);
    if (m.equal_states(t)) {
      assert(result.break_status.approaching_ticks_target);
      assert(t->tick_count() == start_ticks);
      // We didn't make any progress that way.
      // Normally we should just give up now and let reverse_continue keep
      // running and hitting breakpoints etc since we're pretty close to the
      // target already and the overhead of what we have to do here otherwise
      // can be high. But there's a pathological case where reverse_continue
      // is hitting a breakpoint on each iteration of a string instruction.
      // If that's happening then we will be told to force progress.
      if (force == FORCE_PROGRESS) {
        // Let's try a fast-forward singlestep to jump over an x86 string
        // instruction that may be triggering a lot of breakpoint hits. Make
        // sure
        // we stop before |end|.
        ReplaySession::shr_ptr tmp_session;
        if (start_ticks + 1 >= end_ticks) {
          // This singlestep operation might leave us at |end|, which is not
          // allowed. So make a backup of the current state.
          tmp_session = current->clone();
          LOG(debug) << "Created backup tmp_session";
        }
        constraints =
            ReplaySession::StepConstraints(RUN_SINGLESTEP_FAST_FORWARD);
        constraints.stop_before_states.push_back(&end.ptr->regs);
        result = current->replay_step(constraints);
        if (at_mark(end)) {
          assert(tmp_session);
          current = move(tmp_session);
          LOG(debug) << "Singlestepping arrived at |end|, restoring session";
        } else if (!m.equal_states(t)) {
          LOG(debug) << "Did fast-singlestep forward to " << current_mark_key();
          return true;
        }
      }
    } else {
      while (t->tick_count() < target &&
             !result.break_status.approaching_ticks_target) {
        result = current->replay_step(constraints);
      }
      LOG(debug) << "Ran forward to " << current_mark_key();
      return true;
    }
  }

  LOG(debug) << "Made no progress";
  return false;
}

/**
 * Don't allow more than this number of breakpoint/watchpoint stops
 * in a given replay interval. If we hit more than this, try to split
 * the interval in half and replay with watchpoints/breakpoints in the latter
 * half.
 */
static const int stop_count_limit = 20;

ReplayResult ReplayTimeline::reverse_continue(
    std::function<bool()> interrupt_check) {
  Mark end = mark();
  LOG(debug) << "ReplayTimeline::reverse_continue from " << end;

  bool last_stop_is_watch_or_signal;
  ReplayResult final_result;
  TaskUid final_tuid;
  Mark dest;
  vector<Mark> restart_points;

  while (!dest) {
    Mark start = mark();
    bool checkpoint_at_first_break;
    if (start >= end) {
      checkpoint_at_first_break = true;
      if (restart_points.empty()) {
        seek_to_before_key(end.ptr->key);
        start = mark();
        if (start >= end) {
          LOG(debug) << "Couldn't seek to before " << end << ", returning exit";
          // Can't go backwards. Call this an exit.
          final_result.status = REPLAY_EXITED;
          final_result.break_status = BreakStatus();
          return final_result;
        }
        LOG(debug) << "Seeked backward from " << end << " to " << start;
      } else {
        Mark seek = restart_points.back();
        restart_points.pop_back();
        seek_to_mark(seek);
        LOG(debug) << "Seeked directly backward from " << start << " to "
                   << seek;
        start = move(seek);
      }
    } else {
      checkpoint_at_first_break = false;
    }
    maybe_add_reverse_exec_checkpoint(EXPECT_SHORT_REVERSE_EXECUTION);

    bool at_breakpoint = false;
    ReplayStepToMarkStrategy strategy;
    int stop_count = 0;
    bool made_progress_between_stops = false;
    remote_code_ptr avoidable_stop_ip;
    Ticks avoidable_stop_ticks = 0;
    while (true) {
      apply_breakpoints_and_watchpoints();
      ReplayResult result;
      if (at_breakpoint) {
        result = singlestep_with_breakpoints_disabled();
      } else {
        result = replay_step_to_mark(end, strategy);
        // This will remove all reverse-exec checkpoints ahead of the
        // current time, and add new ones if necessary. This should be
        // helpful if we have to reverse-continue far back in time, where
        // the interval between 'start' and 'end' could be lengthy; we'll
        // populate the interval with new checkpoints, speeding up
        // the following seek and possibly future operations.
      }
      at_breakpoint = result.break_status.breakpoint_hit;
      bool avoidable_stop = result.break_status.breakpoint_hit ||
                            !result.break_status.watchpoints_hit.empty();
      if (avoidable_stop) {
        made_progress_between_stops =
            avoidable_stop_ip != result.break_status.task->ip() ||
            avoidable_stop_ticks != result.break_status.task->tick_count();
        avoidable_stop_ip = result.break_status.task->ip();
        avoidable_stop_ticks = result.break_status.task->tick_count();
      }
      evaluate_conditions(result);
      maybe_add_reverse_exec_checkpoint(EXPECT_SHORT_REVERSE_EXECUTION);
      if (checkpoint_at_first_break && dest != start &&
          result.break_status.any_break()) {
        checkpoint_at_first_break = false;
        set_short_checkpoint();
      }

      if (!result.break_status.watchpoints_hit.empty() ||
          result.break_status.signal) {
        dest = mark();
        LOG(debug) << "Found watch/signal break at " << dest;
        final_result = result;
        final_tuid = result.break_status.task ? result.break_status.task->tuid()
                                              : TaskUid();
        last_stop_is_watch_or_signal = true;
      }
      assert(result.status == REPLAY_CONTINUE);

      if (is_start_of_reverse_execution_barrier_event()) {
        dest = mark();
        final_result = result;
        final_result.break_status.task = current->current_task();
        final_result.break_status.task_exit = true;
        final_tuid = final_result.break_status.task->tuid();
        last_stop_is_watch_or_signal = false;
      }

      if (at_mark(end)) {
        // In the next iteration, retry from an earlier checkpoint.
        end = start;
        break;
      }

      // If there is a breakpoint at the current ip() where we start a
      // reverse-continue, gdb expects us to skip it.
      if (result.break_status.breakpoint_hit) {
        dest = mark();
        LOG(debug) << "Found breakpoint break at " << dest;
        final_result = result;
        final_tuid = result.break_status.task ? result.break_status.task->tuid()
                                              : TaskUid();
        last_stop_is_watch_or_signal = false;
      }

      if (interrupt_check()) {
        LOG(debug) << "Interrupted at " << end;
        seek_to_mark(end);
        final_result = ReplayResult();
        final_result.break_status.task = current->current_task();
        return final_result;
      }

      if (avoidable_stop) {
        ++stop_count;
        if (stop_count > stop_count_limit) {
          Mark before_running = mark();
          if (run_forward_to_intermediate_point(end, made_progress_between_stops
                                                         ? DONT_FORCE_PROGRESS
                                                         : FORCE_PROGRESS)) {
            assert(!at_mark(end));
            // We made some progress towards |end| with breakpoints/watchpoints
            // disabled, without reaching |end|. Continuing running forward from
            // here with breakpoints/watchpoints enabled. If we need to seek
            // backwards again, try resuming from the point where we disabled
            // breakpoints/watchpoints.
            if (dest) {
              restart_points.push_back(start);
            }
            restart_points.push_back(before_running);
            dest = Mark();
            break;
          }
        }
      }
    }
  }

  if (last_stop_is_watch_or_signal) {
    LOG(debug)
        << "Performing final reverse-singlestep to pass over watch/signal";
    reverse_singlestep(dest, final_tuid);
  } else {
    LOG(debug) << "Seeking to final destination " << dest;
    seek_to_mark(dest);
  }
  // fix break_status.task since the actual Task* may have changed
  // since we saved final_result
  final_result.break_status.task = current->find_task(final_tuid);
  // Hide any singlestepping we did, since a continue operation should
  // never return a singlestep status
  final_result.break_status.singlestep_complete = false;
  return final_result;
}

ReplayResult ReplayTimeline::reverse_singlestep(const Mark& origin,
                                                const TaskUid& tuid) {
  LOG(debug) << "ReplayTimeline::reverse_singlestep from " << origin;

  while (true) {
    Mark end = origin;
    while (true) {
      MarkKey current_key = end.ptr->key;

      while (true) {
        if (end.ptr->key.trace_time != current_key.trace_time ||
            end.ptr->key.ticks != current_key.ticks) {
          break;
        }
        seek_to_before_key(current_key);
        maybe_add_reverse_exec_checkpoint(EXPECT_SHORT_REVERSE_EXECUTION);
        if (current_mark_key() == current_key) {
          // Can't go further back. Treat this as an exit.
          LOG(debug) << "Couldn't seek to before " << end << ", returning exit";
          ReplayResult result;
          result.status = REPLAY_EXITED;
          result.break_status = BreakStatus();
          return result;
        }
        LOG(debug) << "Seeked backward from " << current_key << " to "
                   << current_mark_key();
        current_key = current_mark_key();
      }

      Mark start = mark();
      LOG(debug) << "Running forward from " << start;
      // Now run forward until we're reasonably close to the correct tick value.
      ReplaySession::StepConstraints constraints(RUN_CONTINUE);
      constraints.ticks_target = end.ptr->key.ticks - 1;
      bool approaching_ticks_target = false;
      while (true) {
        unapply_breakpoints_and_watchpoints();

        if (current_mark() == end.ptr) {
          break;
        }

        Task* t = current->current_task();
        if (t->tuid() == tuid) {
          if (current->can_validate() &&
              t->tick_count() >= end.ptr->key.ticks - 1) {
            // Don't step any further.
            approaching_ticks_target = true;
            break;
          }
          ReplayResult result;
          result = current->replay_step(constraints);
          if (result.break_status.approaching_ticks_target) {
            LOG(debug) << "   approached ticks target at "
                       << current_mark_key();
            constraints =
                ReplaySession::StepConstraints(RUN_SINGLESTEP_FAST_FORWARD);
          }
        } else {
          current->replay_step(RUN_CONTINUE);
        }
        if (is_start_of_reverse_execution_barrier_event()) {
          break;
        }
        maybe_add_reverse_exec_checkpoint(EXPECT_SHORT_REVERSE_EXECUTION);
      }

      if (approaching_ticks_target ||
          is_start_of_reverse_execution_barrier_event()) {
        break;
      }
      end = start;
    }
    assert(current->current_task()->tuid() == tuid);

    Mark destination_candidate;
    Mark step_start = set_short_checkpoint();
    ReplayResult destination_candidate_result;

    if (is_start_of_reverse_execution_barrier_event()) {
      destination_candidate = mark();
      destination_candidate_result.break_status.task_exit = true;
    }

    no_watchpoints_hit_interval_start = Mark();
    while (true) {
      Mark now;
      ReplayResult result;
      if (current->current_task()->tuid() == tuid) {
        apply_breakpoints_and_watchpoints();
        Mark before_step = mark();
        ReplaySession::StepConstraints constraints(RUN_SINGLESTEP_FAST_FORWARD);
        constraints.stop_before_states.push_back(&end.ptr->regs);
        result = current->replay_step(constraints);
        if (result.break_status.breakpoint_hit) {
          unapply_breakpoints_and_watchpoints();
          result = current->replay_step(constraints);
        }
        now = mark();
        if (result.break_status.singlestep_complete) {
          mark_after_singlestep(before_step, result);
          if (now > end) {
            // This last step is not usable.
            LOG(debug) << "   not usable, stopping now";
            break;
          }
          destination_candidate = step_start;
          destination_candidate_result = result;
          step_start = now;
          if (!no_watchpoints_hit_interval_start ||
              !result.break_status.watchpoints_hit.empty()) {
            no_watchpoints_hit_interval_start = now;
          }
        }
      } else {
        unapply_breakpoints_and_watchpoints();
        result = current->replay_step(RUN_CONTINUE);
        no_watchpoints_hit_interval_start = Mark();
        now = mark();
      }

      if (is_start_of_reverse_execution_barrier_event()) {
        destination_candidate = mark();
        destination_candidate_result = result;
        destination_candidate_result.break_status.task_exit = true;
      }

      if (now >= end) {
        break;
      }
      maybe_add_reverse_exec_checkpoint(EXPECT_SHORT_REVERSE_EXECUTION);
    }
    no_watchpoints_hit_interval_end =
        no_watchpoints_hit_interval_start ? end : Mark();

    if (destination_candidate) {
      LOG(debug) << "Found destination " << destination_candidate;
      seek_to_mark(destination_candidate);
      destination_candidate_result.break_status.task = current->find_task(tuid);
      assert(destination_candidate_result.break_status.task);
      evaluate_conditions(destination_candidate_result);
      return destination_candidate_result;
    }
  }
}

void ReplayTimeline::evaluate_conditions(ReplayResult& result) {
  Task* t = result.break_status.task;
  if (!t) {
    return;
  }
  auto auid = t->vm()->uid();

  if (result.break_status.breakpoint_hit) {
    auto addr = t->ip();
    auto it = breakpoints.lower_bound(make_tuple(auid, addr, nullptr));
    bool hit = false;
    while (it != breakpoints.end() && get<0>(*it) == auid &&
           get<1>(*it) == addr) {
      const unique_ptr<BreakpointCondition>& cond = get<2>(*it);
      if (!cond || cond->evaluate(t)) {
        hit = true;
        break;
      }
      ++it;
    }
    if (!hit) {
      result.break_status.breakpoint_hit = false;
    }
  }

  for (auto i = result.break_status.watchpoints_hit.begin();
       i != result.break_status.watchpoints_hit.end(); ++i) {
    auto& w = *i;
    auto it = watchpoints.lower_bound(
        make_tuple(auid, w.addr, w.num_bytes, w.type, nullptr));
    bool hit = false;
    while (it != watchpoints.end() && get<0>(*it) == auid &&
           get<1>(*it) == w.addr && get<2>(*it) == w.num_bytes &&
           get<3>(*it) == w.type) {
      const unique_ptr<BreakpointCondition>& cond = get<4>(*it);
      if (!cond || cond->evaluate(t)) {
        hit = true;
        break;
      }
      ++it;
    }
    if (!hit) {
      i = result.break_status.watchpoints_hit.erase(i);
    }
  }
}

ReplayResult ReplayTimeline::replay_step(
    RunCommand command, RunDirection direction, TraceFrame::Time stop_at_time,
    std::function<bool()> interrupt_check) {
  assert(command != RUN_SINGLESTEP_FAST_FORWARD);

  ReplayResult result;
  if (direction == RUN_FORWARD) {
    apply_breakpoints_and_watchpoints();
    ProtoMark before = proto_mark();
    current->set_visible_execution(true);
    ReplaySession::StepConstraints constraints(command);
    constraints.stop_at_time = stop_at_time;
    result = current->replay_step(constraints);
    current->set_visible_execution(false);
    if (command == RUN_CONTINUE) {
      // Since it's easy for us to fix the coalescing quirk for forward
      // execution, we may as well do so. It's nice to have forward execution
      // behave consistently with reverse execution.
      fix_watchpoint_coalescing_quirk(result, before);
      // Hide any singlestepping we did
      result.break_status.singlestep_complete = false;
    }
    maybe_add_reverse_exec_checkpoint(LOW_OVERHEAD);

    bool did_hit_breakpoint = result.break_status.breakpoint_hit;
    evaluate_conditions(result);
    if (did_hit_breakpoint && !result.break_status.any_break()) {
      // Singlestep past the breakpoint
      current->set_visible_execution(true);
      result = singlestep_with_breakpoints_disabled();
      if (command == RUN_CONTINUE) {
        result.break_status.singlestep_complete = false;
      }
      current->set_visible_execution(false);
    }
  } else {
    assert(stop_at_time == 0 &&
           "stop_at_time unsupported for reverse execution");

    switch (command) {
      case RUN_CONTINUE:
        result = reverse_continue(interrupt_check);
        break;
      case RUN_SINGLESTEP:
        result = reverse_singlestep(mark(), current->current_task()->tuid());
        break;
      default:
        assert(0 && "Unknown RunCommand");
        return ReplayResult();
    }
  }
  return result;
}

ReplayTimeline::Progress ReplayTimeline::estimate_progress() {
  Session::Statistics stats = current->statistics();
  // The following parameters were estimated by running Firefox startup
  // and shutdown in an opt build on a Lenovo W530 laptop, replaying with
  // DUMP_STATS_PERIOD set to 100 (twice, and using only values from the
  // second run, to ensure caches are warm), and then minimizing least-squares
  // error.
  static const double microseconds_per_tick = 0.0020503143;
  static const double microseconds_per_syscall = 39.6793587609;
  static const double microseconds_per_byte_written = 0.001833611;
  static const double microseconds_constant = 997.8257239043;
  return Progress(microseconds_per_tick * stats.ticks_processed +
                  microseconds_per_syscall * stats.syscalls_performed +
                  microseconds_per_byte_written * stats.bytes_written +
                  microseconds_constant);
}

/*
 * Checkpointing strategy:
 *
 * We define a series of intervals of increasing length, each one ending at
 * the current replay position. In each interval N, we allow at most N
 * checkpoints. We ensure that interval lengths grow exponentially (in the
 * limit), so the maximum number of checkpoints for a given execution length
 * L is O(log L).
 *
 * Interval N has length inter_checkpoint_interval to the
 * power of checkpoint_interval_exponent.
 * We allow at most N checkpoints in interval N.
 * To discard excess checkpoints, first pick the smallest interval N with
 * too many checkpoints, and discard the latest checkpoint in interval N
 * that is not in interval N-1. Repeat until there are no excess checkpoints.
 * All checkpoints after the current replay point are always discarded.
 * The script checkpoint-visualizer.html simulates this algorithm and
 * visualizes its results.
 * The implementation here is quite naive, but that's OK because we will
 * never have a large number of checkpoints.
 */

/**
 * Try to space out our checkpoints by a minimum of this much in LOW_OVERHEAD
 * mode.
 * This is currently aiming for about 0.5s of replay time, so a reverse step or
 * continue whose destination is within 0.5 should take at most a second.
 * Also, based on a guesstimate that taking checkpoints of Firefox requires
 * about 50ms, this would make checkpointing overhead about 10% of replay time,
 * which sounds reasonable.
 */
static ReplayTimeline::Progress low_overhead_inter_checkpoint_interval = 500000;

/**
 * Space out checkpoints linearly by this much in
 * EXPECT_SHORT_REVERSE_EXECUTION mode, until we reach
 * low_overhead_inter_checkpoint_interval.
 */
static ReplayTimeline::Progress
    expecting_reverse_exec_inter_checkpoint_interval = 100000;

/**
 * Make each interval this much bigger than the previous.
 */
static float checkpoint_interval_exponent = 2;

static ReplayTimeline::Progress inter_checkpoint_interval(
    ReplayTimeline::CheckpointStrategy strategy) {
  return strategy == ReplayTimeline::LOW_OVERHEAD
             ? low_overhead_inter_checkpoint_interval
             : expecting_reverse_exec_inter_checkpoint_interval;
}

static ReplayTimeline::Progress next_interval_length(
    ReplayTimeline::Progress len) {
  if (len >= low_overhead_inter_checkpoint_interval) {
    return (ReplayTimeline::Progress)ceil(checkpoint_interval_exponent * len);
  }
  return len + expecting_reverse_exec_inter_checkpoint_interval;
}

void ReplayTimeline::maybe_add_reverse_exec_checkpoint(
    CheckpointStrategy strategy) {
  discard_future_reverse_exec_checkpoints();

  Progress now = estimate_progress();
  auto it = reverse_exec_checkpoints.rbegin();
  if (it != reverse_exec_checkpoints.rend() &&
      it->second >= now - inter_checkpoint_interval(strategy)) {
    // Latest checkpoint is close enough; we don't need to do anything.
    return;
  }

  if (!current->can_clone()) {
    // We can't create a checkpoint right now.
    return;
  }

  // We always discard checkpoints before adding the new one to reduce the
  // maximum checkpoint count by one.
  discard_past_reverse_exec_checkpoints(strategy);

  reverse_exec_checkpoints[add_explicit_checkpoint()] = now;
}

void ReplayTimeline::discard_future_reverse_exec_checkpoints() {
  Progress now = estimate_progress();
  while (true) {
    auto it = reverse_exec_checkpoints.rbegin();
    if (it == reverse_exec_checkpoints.rend() || it->second < now) {
      break;
    }
    remove_explicit_checkpoint(it->first);
    reverse_exec_checkpoints.erase(it->first);
  }
}

void ReplayTimeline::discard_past_reverse_exec_checkpoints(
    CheckpointStrategy strategy) {
  Progress now = estimate_progress();
  // No checkpoints are allowed in the first interval, since we're about to
  // add one there.
  int checkpoints_allowed = 0;
  int checkpoints_in_range = 0;
  auto it = reverse_exec_checkpoints.rbegin();
  vector<Mark> checkpoints_to_delete;
  for (Progress len = inter_checkpoint_interval(strategy);;
       len = next_interval_length(len)) {
    Progress start = now - len;
    // Count checkpoints >= start, starting at 'it', and leave the first
    // checkpoint entry < start in 'tmp_it'.
    auto tmp_it = it;
    while (tmp_it != reverse_exec_checkpoints.rend() &&
           tmp_it->second >= start) {
      ++checkpoints_in_range;
      ++tmp_it;
    }
    // Delete excess checkpoints starting with 'it'.
    while (checkpoints_in_range > checkpoints_allowed) {
      checkpoints_to_delete.push_back(it->first);
      --checkpoints_in_range;
      ++it;
    }
    ++checkpoints_allowed;
    it = tmp_it;
    if (it == reverse_exec_checkpoints.rend()) {
      break;
    }
  }

  for (auto& m : checkpoints_to_delete) {
    remove_explicit_checkpoint(m);
    reverse_exec_checkpoints.erase(m);
  }
}

ReplayTimeline::Mark ReplayTimeline::set_short_checkpoint() {
  if (!can_add_checkpoint()) {
    return mark();
  }

  // Add checkpoint before removing one in case m ==
  // reverse_exec_short_checkpoint
  Mark m = add_explicit_checkpoint();
  if (reverse_exec_short_checkpoint) {
    remove_explicit_checkpoint(reverse_exec_short_checkpoint);
  }
  swap(m, reverse_exec_short_checkpoint);
  return reverse_exec_short_checkpoint;
}
