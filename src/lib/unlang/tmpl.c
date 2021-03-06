/*
 *   This program is free software; you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation; either version 2 of the License, or
 *   (at your option) any later version.
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with this program; if not, write to the Free Software
 *   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

/**
 * $Id$
 *
 * @file unlang/tmpl.c
 * @brief Defines functions for calling vp_tmpl_t asynchronously
 *
 * @copyright 2020 Network RADIUS SARL <legal@networkradius.com>
 */

RCSID("$Id$")

#include <freeradius-devel/unlang/base.h>
#include <freeradius-devel/unlang/tmpl.h>
#include <freeradius-devel/server/exec.h>
#include "tmpl_priv.h"
#include <signal.h>

/*
 *	Clean up everything except the waitpid handler.
 *
 *	If there is a waitpid handler, then this cleanup function MUST
 *	be called after setting the handler.
 */
static void unlang_tmpl_exec_cleanup(REQUEST *request)
{
	unlang_stack_t			*stack = request->stack;
	unlang_stack_frame_t		*frame = &stack->frame[stack->depth];
	unlang_frame_state_tmpl_t	*state = talloc_get_type_abort(frame->state,
								       unlang_frame_state_tmpl_t);

	if (state->fd >= 0) {
		(void) fr_event_fd_delete(request->el, state->fd, FR_EVENT_FILTER_IO);
		close(state->fd);
		state->fd = -1;
	}

	if (state->pid) fr_exec_waitpid(state->pid);

	if (state->ev) fr_event_timer_delete(&state->ev);
}

/** Send a signal (usually stop) to a request
 *
 * This is typically called via an "async" action, i.e. an action
 * outside of the normal processing of the request.
 *
 * If there is no #fr_unlang_tmpl_signal_t callback defined, the action is ignored.
 *
 * @param[in] request		The current request.
 * @param[in] action		to signal.
 */
static void unlang_tmpl_signal(REQUEST *request, fr_state_signal_t action)
{
	unlang_stack_t			*stack = request->stack;
	unlang_stack_frame_t		*frame = &stack->frame[stack->depth];
	unlang_frame_state_tmpl_t	*state = talloc_get_type_abort(frame->state,
								       unlang_frame_state_tmpl_t);

	if (!state->signal) return;

	state->signal(request, state->rctx, action);

	/*
	 *	If we're cancelled, then kill any child processes, and
	 *	ignore future signals.
	 */
	if (action == FR_SIGNAL_CANCEL) {
		if (state->buffer) unlang_tmpl_exec_cleanup(request);
		state->signal = NULL;
	}
}

/** Push a tmpl onto the stack for evaluation
 *
 * @param[in] ctx		To allocate value boxes and values in.
 * @param[out] out		The value_box created from the tmpl.  May be NULL,
 *				in which case the result is discarded.
 * @param[in] request		The current request.
 * @param[in] tmpl		the tmpl to expand
 */
void unlang_tmpl_push(TALLOC_CTX *ctx, fr_value_box_t **out, REQUEST *request, vp_tmpl_t const *tmpl)
{
	unlang_stack_t			*stack = request->stack;
	unlang_stack_frame_t		*frame = &stack->frame[stack->depth];
	unlang_frame_state_tmpl_t	*state = talloc_get_type_abort(frame->state,
								       unlang_frame_state_tmpl_t);
	unlang_tmpl_t			*ut;

	static unlang_t tmpl_instruction = {
		.type = UNLANG_TYPE_TMPL,
		.name = "tmpl",
		.debug_name = "tmpl",
		.actions = {
			[RLM_MODULE_REJECT]	= 0,
			[RLM_MODULE_FAIL]	= 0,
			[RLM_MODULE_OK]		= 0,
			[RLM_MODULE_HANDLED]	= 0,
			[RLM_MODULE_INVALID]	= 0,
			[RLM_MODULE_DISALLOW]	= 0,
			[RLM_MODULE_NOTFOUND]	= 0,
			[RLM_MODULE_NOOP]	= 0,
			[RLM_MODULE_UPDATED]	= 0
		},
	};

	state->out = out;
	state->ctx = ctx;

	MEM(ut = talloc(state, unlang_tmpl_t));
	ut->self = tmpl_instruction;
	ut->tmpl = tmpl;

	/*
	 *	Push a new tmpl frame onto the stack
	 */
	unlang_interpret_push(request, unlang_tmpl_to_generic(ut), RLM_MODULE_UNKNOWN, UNLANG_NEXT_STOP, false);
}


static void unlang_tmpl_exec_waitpid(UNUSED fr_event_list_t *el, UNUSED pid_t pid, int status, void *uctx)
{
	REQUEST				*request = uctx;
	unlang_stack_t			*stack = request->stack;
	unlang_stack_frame_t		*frame = &stack->frame[stack->depth];
	unlang_frame_state_tmpl_t	*state = talloc_get_type_abort(frame->state,
								       unlang_frame_state_tmpl_t);

	state->status = status;

	rad_assert(state->pid == 0);
	rad_assert(state->fd < 0);
	rad_assert(state->ev == NULL);
	unlang_interpret_resumable(request);
}


static void unlang_tmpl_exec_read(UNUSED fr_event_list_t *el, int fd, UNUSED int flags, void *uctx)
{
	REQUEST				*request = uctx;
	unlang_stack_t			*stack = request->stack;
	unlang_stack_frame_t		*frame = &stack->frame[stack->depth];
	unlang_frame_state_tmpl_t	*state = talloc_get_type_abort(frame->state,
								       unlang_frame_state_tmpl_t);
	ssize_t data_len;
	char *p, *end;

	p = state->ptr;
	end = state->buffer + talloc_array_length(state->buffer);

	data_len = read(fd, p, end - p);
	if (data_len < 0) {
		if (errno == EINTR) return;

		unlang_tmpl_exec_cleanup(request);
		unlang_interpret_resumable(request);
		return;
	}

	p += data_len;

	/*
	 *	Done reading, close the pipe.
	 */
	if ((data_len == 0) || (p >= end)) {
		/*
		 *	Ran out of buffer space.  Kill the process.
		 */
		if (p >= end) kill(state->pid, SIGKILL);

		/*
		 *	This event will stick around until the process exits.
		 */
		if (fr_event_pid_wait(state, request->el, &state->ev_pid, state->pid,
				      unlang_tmpl_exec_waitpid, request) < 0) {
			unlang_tmpl_exec_cleanup(request);
			unlang_interpret_resumable(request);
			return;
		}
		state->pid = 0;	/* don't kill the process */

		/*
		 *	Clean up the FD, reader, and timeouts.
		 */
		unlang_tmpl_exec_cleanup(request);

		/*
		 *	Once the process exits, we will be notified, and
		 */
		return;
	}

	rad_assert(p < end);
	state->ptr = p;
}

static void unlang_tmpl_exec_timeout(UNUSED fr_event_list_t *el, UNUSED fr_time_t now, void *uctx)
{
	REQUEST				*request = uctx;

	REDEBUG("Timeout running program - kill it and failing the request");
	unlang_tmpl_exec_cleanup(request);
	unlang_interpret_resumable(request);
}



/** Wrapper to call a resumption function after a tmpl has been expanded
 *
 *  If the resumption function returns YIELD, then this function is
 *  called repeatedly until the resumption function returns a final
 *  value.
 */
static unlang_action_t unlang_tmpl_resume(REQUEST *request, rlm_rcode_t *presult)
{
	unlang_stack_t			*stack = request->stack;
	unlang_stack_frame_t		*frame = &stack->frame[stack->depth];
	unlang_frame_state_tmpl_t	*state = talloc_get_type_abort(frame->state,
								       unlang_frame_state_tmpl_t);

	if (state->out) *state->out = state->box;

	if (state->resume) {
		rlm_rcode_t rcode;

		rcode = state->resume(request, state->rctx);
		*presult = rcode;
		if (rcode == RLM_MODULE_YIELD) return UNLANG_ACTION_YIELD;
	} else {
		*presult = RLM_MODULE_OK;
	}

	return UNLANG_ACTION_CALCULATE_RESULT;
}



/** Wrapper to call exec after the program has finished executing
 *
 */
static unlang_action_t unlang_tmpl_exec_wait_final(REQUEST *request, rlm_rcode_t *presult)
{
	unlang_stack_t			*stack = request->stack;
	unlang_stack_frame_t		*frame = &stack->frame[stack->depth];
	unlang_frame_state_tmpl_t	*state = talloc_get_type_abort(frame->state,
								       unlang_frame_state_tmpl_t);

	if (state->status != 0) {
		*presult = RLM_MODULE_FAIL;
		return UNLANG_ACTION_CALCULATE_RESULT;
	}

	/*
	 *	Ensure that the callers resume function is called.
	 */
	frame->interpret = unlang_tmpl_resume;
	return unlang_tmpl_resume(request, presult);
}


/** Wrapper to call exec after a tmpl has been expanded
 *
 */
static unlang_action_t unlang_tmpl_exec_wait_resume(REQUEST *request, rlm_rcode_t *presult)
{
	unlang_stack_t			*stack = request->stack;
	unlang_stack_frame_t		*frame = &stack->frame[stack->depth];
	unlang_frame_state_tmpl_t	*state = talloc_get_type_abort(frame->state,
								       unlang_frame_state_tmpl_t);
	int				fd;
	pid_t				pid;

	fd = fr_exec_wait_start(request, state->box, NULL, &pid);
	if (fd < 0) {
		REDEBUG("Failed executing program - %s", fr_strerror());
	fail:
		*presult = RLM_MODULE_FAIL;
		return UNLANG_ACTION_CALCULATE_RESULT;
	}

	state->fd = fd;
	state->pid = pid;
	state->status = 1;	/* default to program didn't work */
	MEM(state->buffer = talloc_array(state, char, 024));

	/*
	 *	Kill the child process after a period of time.
	 *
	 *	@todo - make the timeout configurable
	 */
	if (fr_event_timer_in(request, request->el, &state->ev, 10 * NSEC, unlang_tmpl_exec_timeout, request) < 0) {
		unlang_tmpl_exec_cleanup(request);
		goto fail;
	}

	if (fr_event_fd_insert(state->ctx, request->el, state->fd, unlang_tmpl_exec_read, NULL, NULL, request) < 0) {
		REDEBUG("Failed adding event - %s", fr_strerror());
		unlang_tmpl_exec_cleanup(request);
		goto fail;
	}

	frame->interpret = unlang_tmpl_exec_wait_final;

	*presult = RLM_MODULE_YIELD;
	return UNLANG_ACTION_YIELD;

}

/** Wrapper to call exec after a tmpl has been expanded
 *
 */
static unlang_action_t unlang_tmpl_exec_nowait_resume(REQUEST *request, rlm_rcode_t *presult)
{
	unlang_stack_t			*stack = request->stack;
	unlang_stack_frame_t		*frame = &stack->frame[stack->depth];
	unlang_frame_state_tmpl_t	*state = talloc_get_type_abort(frame->state,
								       unlang_frame_state_tmpl_t);

	if (fr_exec_nowait(request, state->box, NULL) < 0) {
		REDEBUG("Failed executing program - %s", fr_strerror());
		*presult = RLM_MODULE_FAIL;

	} else {
		*presult = RLM_MODULE_OK;
	}

	/*
	 *	state->resume MUST be NULL, as we don't yet support
	 *	exec from unlang_tmpl_push().
	 */

	return UNLANG_ACTION_CALCULATE_RESULT;
}


static unlang_action_t unlang_tmpl(REQUEST *request, rlm_rcode_t *presult)
{
	unlang_stack_t			*stack = request->stack;
	unlang_stack_frame_t		*frame = &stack->frame[stack->depth];
	unlang_frame_state_tmpl_t	*state = talloc_get_type_abort(frame->state,
								       unlang_frame_state_tmpl_t);
	unlang_tmpl_t			*ut = unlang_generic_to_tmpl(frame->instruction);
	xlat_exp_t const		*xlat;

	/*
	 *	If we're not called from unlang_tmpl_push(), then
	 *	ensure that we clean up the resulting value boxes.
	 */
	if (!state->ctx) state->ctx = state;

	if (!tmpl_async_required(ut->tmpl)) {
		if (!ut->inline_exec) {
			if (tmpl_aexpand_type(state->ctx, &state->box, FR_TYPE_STRING, request, ut->tmpl, NULL, NULL) < 0) {
				REDEBUG("Failed expanding %s - %s", ut->tmpl->name, fr_strerror());
				*presult = RLM_MODULE_FAIL;
			}

			*presult = RLM_MODULE_OK;
			return UNLANG_ACTION_CALCULATE_RESULT;
		}

		/*
		 *	Inline exec's are only called from in-line
		 *	text in the configuration files.
		 */
		frame->interpret = unlang_tmpl_exec_nowait_resume;

		repeatable_set(frame);
		unlang_xlat_push(state->ctx, &state->box, request, ut->tmpl->tmpl_xlat, false);
		return UNLANG_ACTION_PUSHED_CHILD;
	}

	/*
	 *	XLAT structs are allowed.
	 */
	if (ut->tmpl->type == TMPL_TYPE_XLAT_STRUCT) {
		frame->interpret = unlang_tmpl_resume;
		repeatable_set(frame);
		unlang_xlat_push(state->ctx, &state->box, request, ut->tmpl->tmpl_xlat, false);
		return UNLANG_ACTION_PUSHED_CHILD;
	}

	if (ut->tmpl->type == TMPL_TYPE_XLAT) {
		REDEBUG("Xlat expansions MUST be compiled before being run asynchronously");
		*presult = RLM_MODULE_FAIL;
		return UNLANG_ACTION_CALCULATE_RESULT;
	}

	/*
	 *	Attribute expansions, etc. don't require YIELD.
	 */
	if (ut->tmpl->type != TMPL_TYPE_EXEC) {
		REDEBUG("Internal error - template '%s' should not require async", ut->tmpl->name);
	fail:
		*presult = RLM_MODULE_FAIL;
		return UNLANG_ACTION_CALCULATE_RESULT;
	}

	/*
	 *	No pre-parsed xlat, die.
	 */
	if (!ut->tmpl->tmpl_xlat) {
		ssize_t slen;
		xlat_exp_t *head = NULL;

		slen = xlat_tokenize(state->ctx, &head, ut->tmpl->name, talloc_array_length(ut->tmpl->name) - 1, NULL);
		if (slen <= 0) {
			char *spaces, *text;

			fr_canonicalize_error(state->ctx, &spaces, &text, slen, ut->tmpl->name);
			REDEBUG("Failed parsing expansion string:");
			REDEBUG("%s", text);
			REDEBUG("%s^ %s", spaces, fr_strerror());

			talloc_free(spaces);
			talloc_free(text);
			goto fail;
		}

		xlat = head;
	} else {
		xlat = ut->tmpl->tmpl_xlat;
	}

	/*
	 *	Expand the arguments to the program we're executing.
	 */
	frame->interpret = unlang_tmpl_exec_wait_resume;
	repeatable_set(frame);
	unlang_xlat_push(state->ctx, &state->box, request, xlat, false);
	return UNLANG_ACTION_PUSHED_CHILD;
}


void unlang_tmpl_init(void)
{
	unlang_register(UNLANG_TYPE_TMPL,
			   &(unlang_op_t){
				.name = "tmpl",
				.interpret = unlang_tmpl,
				.signal = unlang_tmpl_signal,
				.frame_state_size = sizeof(unlang_frame_state_tmpl_t),
				.frame_state_name = "unlang_frame_state_tmpl_t",
			   });
}
