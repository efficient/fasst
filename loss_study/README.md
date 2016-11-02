# RDMA packet loss study

This is the data and code dump for an RDMA loss test. This directory is also
available in `/proj/fawn/akalia/rdma_losses/` in Apt.

The experiment was run from 18th of July, 2016, 09:07 am (Utah time), to
19th of July, 22:33 pm, i.e., for approximately 37 hours and 30 minutes. The
experiment was ended because (a) the output logs were close to exceeding the
homedir quota on Apt, and (b) a ridiculous number of packets had been generated.

The experiment was run on Apt using the `code` in this directory. The tested
configuration was:
 * 69 machines, connected to different switches in Apt. The 7 leaf switches on
   Apt hosted 5, 18, 0, 20, 11, 0, and 16 nodes, respectively. One node did
   not boot so was excluded from the test.
 * 8 threads per machine, for a total of 552 threads in the cluster
 * 256-byte requests and responses
 * Each thread sends out a batch of 32 requests and waits for all responses to
   arrive before starting the next batch
 * Each thread verifies the response size, and the "token" field in the immediate
   header. The payloads were not verified, but they are guaranteed to be
   uncorrupted.

The output was recorded into two directories. Unpack them using:
```
	tar xf out.lzma
	tar xf losses.lzma
```

## Statistics
 * Each machine generated approximately 600 billion requests. Per machine output
   can for machine N be seen in:
    * `out/out-machine-N`: Contains throughput and total requests generated.
    * `losses/out-machine-8*N`: Contains machine throughput, per-thread
       throughput, and real timestamps. No thread has zero throughput, which
       means that all requests received valid responses until the end of
       experiment.

 * Overall, 42.2 trillion requests were generated. This can be computed using
   the `losses/sum-reqs.sh` script as follows:
```
     ./sum-reqs.sh 552
```
   Including the 8 trillion requests generated in a previous run (7.1 hours),
   the total number of requests is 50 trillion. These hours are included in the
   paper.

 * 1557 reordering events were detected. This can be computed using the
   `losses/count-errors.sh` script as follows:
```
     ./count-errors.sh
```
   The output of `count-errors.sh` should be divided by 3 -- each reordering
   event leads to 3 lines of output. The reordering events observed at machine
   N are recorded in `out/err-machine-N`.

 * Each reordering event involved a sequence of tokens such as: `{N, N-1, N+1}`.
   The token `N-1` is reordered.

 * The 1557 reordering events took place at 57 unique timestamps, measured at
   a seconds granularity. This can be computed using the `losses/show-errors.sh`
   script as follows:
```
     ./show-errors.sh | wc -l
```

## Notes
 * The per-machine throughput in the experiment was not a concern and was not
   optimized for. It is much lower than the per-machine limit because the
   cluster is oversubscribed, and this experiment actually stressed the
   oversubscription.
