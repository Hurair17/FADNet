import os
import subprocess

from .base import NetworkBackend


class Ns3NdnBackend(NetworkBackend):

    def __init__(
        self,
        model_bytes,
        deadline=15.0,
        ns3_dir=None,
        program="gaia-sfl-ndn",
        trace_dir=None,
        trace_prefix="wired_ndn",
        download_extra_args="",
        upload_extra_args="",
    ):
        self.model_bytes = model_bytes
        self.deadline = deadline
        # Falls back to run.sh's exported NS3_DIR so this works on any
        # machine, not just this one; only hardcodes a path for local,
        # non-run.sh usage.
        self.ns3_dir = ns3_dir or os.environ.get(
            "NS3_DIR", "/home/hurair/ndnSIM/ns-3"
        )
        self.program = program
        # Every scenario's trace files live under scratch/<program name>,
        # so derive trace_dir from ns3_dir/program unless overridden.
        self.trace_dir = trace_dir or os.path.join(
            self.ns3_dir, "scratch", program
        )
        self.trace_prefix = trace_prefix
        # Extra CLI args appended to each phase's ns-3 invocation
        # (e.g. "--consumerType=cbr --cbrFrequency=20"). ConsumerCbr's
        # steady, fixed-rate pacing beats ConsumerWindow's bursty AIMD
        # for both phases — download's own burst pattern caused uneven
        # per-silo completion even with only one broadcaster, and
        # upload's multi-concurrent-broadcaster contention needs it
        # even more. Kept as separate, overridable args per phase in
        # case that ever needs to differ again.
        self.download_extra_args = download_extra_args
        self.upload_extra_args = upload_extra_args

        self.payload_size = 4096
        self.total_chunks = (
            model_bytes + self.payload_size - 1
        ) // self.payload_size

        self.last_seq = self.total_chunks - 1

        self.cumulative_network_time = 0.0

    def _run_phase(
        self,
        round_idx,
        phase,
        silo_ids,
    ):
        if len(silo_ids) == 0:
            return {}

        # The ns-3 topology is built fresh for every phase call, sized to
        # exactly the silos active this call — not max(silo_ids) + 1,
        # which would force e.g. active silos [5, 6, 7, 9] into a
        # 10-station topology (creating 6 real WiFi stations — with
        # their own idle association-handshake overhead — that nobody
        # asked for). gaia-sfl-ndn-wifi.cc indexes WiFi stations by
        # position among the active silos (loopIdx), not by real silo
        # ID, so a compact numSilos here is safe.
        sorted_silo_ids = sorted(silo_ids)

        num_silos = len(sorted_silo_ids)

        active_silos_arg = ",".join(
            str(silo_id) for silo_id in sorted_silo_ids
        )

        sim_command = (
            f"{self.program} "
            f"--phase={phase} "
            f"--round={round_idx} "
            f"--numSilos={num_silos} "
            f"--activeSilos={active_silos_arg} "
            f"--modelBytes={self.model_bytes} "
            f"--deadline={self.deadline}"
        )

        extra_args = (
            self.download_extra_args
            if phase == "download"
            else self.upload_extra_args
        )

        if extra_args:
            sim_command += f" {extra_args}"

        print(
            f"[Round {round_idx}] "
            f"ns-3 NDN {phase} START",
            flush=True,
        )

        completed = subprocess.run(
            [
                "./waf",
                "--run",
                sim_command,
            ],
            cwd=self.ns3_dir,
            capture_output=True,
            text=True,
        )

        if completed.returncode != 0:
            print(completed.stdout)
            print(completed.stderr)

            raise RuntimeError(
                f"ns-3 NDN {phase} failed"
            )

        trace_file = os.path.join(
            self.trace_dir,
            f"{self.trace_prefix}_{phase}_app_delay.txt",
        )

        return self._parse_trace(
            trace_file=trace_file,
            phase=phase,
            silo_ids=silo_ids,
            sorted_silo_ids=sorted_silo_ids,
        )

    def _parse_trace(
        self,
        trace_file,
        phase,
        silo_ids,
        sorted_silo_ids,
    ):
        results = {
            silo_id: {
                "success": False,
                "time": self.deadline,
                "bytes": 0,
            }
            for silo_id in silo_ids
        }

        max_seq = {}
        finish_time = {}
        received_chunks = {}

        with open(trace_file, "r") as f:
            header = f.readline()

            for line in f:
                parts = line.split()

                if len(parts) < 9:
                    continue

                time_s = float(parts[0])
                node = int(parts[1])
                app_id = int(parts[2])
                seq_no = int(parts[3])
                record_type = parts[4]

                if record_type != "FullDelay":
                    continue

                if phase == "download":
                    silo_id = node
                else:
                    # Upload's consumers all live on the same node
                    # (the server), so ns-3 assigns their AppIds by
                    # installation order (0, 1, 2, ...) rather than
                    # by real silo ID. gaia-sfl-ndn-wifi.cc installs
                    # them in the same sorted order as
                    # sorted_silo_ids, so AppId N is the Nth
                    # smallest requested silo ID, not silo ID N.
                    if app_id >= len(sorted_silo_ids):
                        continue

                    silo_id = sorted_silo_ids[app_id]

                if silo_id not in results:
                    continue

                received_chunks[silo_id] = (
                    received_chunks.get(silo_id, 0)
                    + 1
                )

                if (
                    silo_id not in max_seq
                    or seq_no > max_seq[silo_id]
                ):
                    max_seq[silo_id] = seq_no

                if seq_no == self.last_seq:
                    finish_time[silo_id] = time_s

        start_time = 1.0

        for silo_id in silo_ids:

            complete = (
                max_seq.get(silo_id, -1)
                >= self.last_seq
                and received_chunks.get(silo_id, 0)
                >= self.total_chunks
            )

            if complete:
                absolute_finish = finish_time.get(
                    silo_id,
                    start_time + self.deadline,
                )

                transfer_time = (
                    absolute_finish - start_time
                )

                success = (
                    transfer_time
                    <= self.deadline
                )

                results[silo_id] = {
                    "success": success,
                    "time": transfer_time,
                    "bytes": (
                        self.model_bytes
                        if success
                        else 0
                    ),
                }

        phase_time = max(
            r["time"]
            for r in results.values()
        )

        self.cumulative_network_time += (
            phase_time
        )

        successful = sum(
            r["success"]
            for r in results.values()
        )

        print(
            f"[NDN {phase}] "
            f"{successful}/{len(silo_ids)} successful "
            f"| phase time={phase_time:.3f}s",
            flush=True,
        )

        for silo_id in silo_ids:
            result = results[silo_id]

            status = (
                "SUCCESS"
                if result["success"]
                else "FAILED"
            )

            print(
                f"    Silo {silo_id}: "
                f"{status} "
                f"| {result['time']:.3f}s "
                f"| bytes={result['bytes']}",
                flush=True,
            )

        return results

    def download(
        self,
        round_idx,
        silo_ids,
    ):
        return self._run_phase(
            round_idx,
            "download",
            silo_ids,
        )

    def upload(
        self,
        round_idx,
        silo_ids,
    ):
        return self._run_phase(
            round_idx,
            "upload",
            silo_ids,
        )