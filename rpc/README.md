# HoTS RPCs

* The RPC subsystem has been specially designed for distributed transactions and
coroutines. The slave coroutines run the transaction processing logic. We
switch in and out of the master and slave coroutine functions only. The RPC
subsystem should not yield.

* SHM key use: The RPC subsystem uses SHM keys `w` for a local worker thread
with ID `w`. With `W` worker threads, SHM keys 1 through `W` are used. Assuming
that we don't run more than 56 worker threads per machine, keys 1 through 56
belong to the RPC subsystem.

* Slave coroutine loop:
  * Clear the RPC message batch: `clear_req_batch(coro_id)`
  * Get a request buffer `rpc_req_t *start_new_req(coro_id, type, resp_wn)`
  * Freeze the request: `rpc_req_t->freeze()`
  * Send the queued requests: `send_reqs(coro_id)`
  * Yield to the next coroutine
  * When control returns to the slave coroutine, there should be a response for
    every request.
  * Process responses

* Master coroutine loop:
  * Poll RECV completions using `poll_comps()`. `poll_comps` should work as
    follows:
    * If a completion is request-type, invoke a handler.
    * If a completion is response-type, add it to the coroutine that initiated
      the request.
    * Send out a batch of responses using the request message batch.
    * Create a `next_coroutine` structure with the coroutines that have finished
	  execution. Return this to the caller (the master coroutine).
 * Use the `next_coroutine` structure to yield to slave coroutines that have a
   full response batch. If `next_coroutine` is empty, go back to the master loop.
 * When control returns to the master coroutine, go back to master loop.

* Handling non-inlined messages:
  * Non-inlined messages require registered buffers. We get these buffers by
    allocating extra memory in `hrd_ctrl_blk_init()`: `RPC_MBUF_SPACE` bytes
    are used for all request and response mbufs, and `RPC_NON_INLINE_SPACE`
    bytes are used for responses that exceed the inline threshold.
  * Requests need no special handling because they are not reused by the RPC
    layer until they are not needed anymore. As request mbufs belong to
    registered memory, they can be transferred inline or non-inline.
  * Responses need special handling because there can be reused. Consider the
    case where a response batch uses response buffers 1 through 10 and the next
    response batch also uses buffers 1 through 10. Assume `RPC_UNSIG_BATCH = 64`.
    In this case, if any of the responses in the 1st batch exceeded the inline
    threshold, the 2nd batch may overwrite the buffer before the NIC DMAs' it.
    * A possible solution is to cycle among response buffers, but that increase
      cache pressure if the cycle length is too large (e.g., `HRD_RQ_DEPTH`).
      Keeping the cycle length small (e.g., `2 * RPC_UNSIG_BATCH`) is a little
      complicated.
    * Our solution: Cycle among specially-allocated `non_inline_bufs` only when
      the response needs to be non-inlined. So when a response is non-inlined,
      we pick the next non-inlined buffer in the cycle and **copy** the response
      there. Doing so uses the first few response buffers (as many as the average
      response batch) in the common case of inlined responses, reducing cache
      pressure.
