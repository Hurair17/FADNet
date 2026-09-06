import os
import subprocess

from .base import NetworkBackend


class Ns3TcpBackend(NetworkBackend):

    def __init__(
        self,
        model_bytes,
        deadline=15.0,
        ns3_dir=None,
        program="gaia-sfl-tcp",
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

        self.cumulative_network_time = 0.0

    def _run_phase(
        self,
        round_idx,
        phase,
        silo_ids,
    ):

        if len(silo_ids) == 0:
            return {}

        silo_string = ",".join(
            str(silo_id)
            for silo_id in silo_ids
        )

        # The ns-3 topology is built fresh for every phase call, sized to
        # cover every silo this run could ever address (not just the
        # active ones this round), so silo IDs stay stable across rounds.
        num_silos = max(silo_ids) + 1

        sim_command = (
            f"{self.program} "
            f"--phase={phase} "
            f"--round={round_idx} "
            f"--numSilos={num_silos} "
            f"--activeSilos={silo_string} "
            f"--modelBytes={self.model_bytes} "
            f"--deadline={self.deadline} "
            f"--logFile={self.ns3_dir}/"
            f"scratch/{self.program}/logs/wired_tcp_network.csv"
        )

        print(
            f"[Round {round_idx}] "
            f"ns-3 TCP {phase} START",
            flush=True
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
            print(
                completed.stdout
            )

            print(
                completed.stderr
            )

            raise RuntimeError(
                f"ns-3 TCP {phase} failed"
            )

        results = {}

        for line in completed.stdout.splitlines():

            if not line.startswith(
                "NET_RESULT"
            ):
                continue

            fields = {}

            for token in line.split()[1:]:

                key, value = token.split(
                    "=",
                    1
                )

                fields[key] = value

            silo_id = int(
                fields["silo"]
            )

            results[silo_id] = {
                "success":
                    int(fields["success"]) == 1,

                "time":
                    float(fields["time"]),

                "bytes":
                    int(fields["bytes"]),
            }

        # Sanity check
        missing = (
            set(silo_ids)
            -
            set(results.keys())
        )

        if missing:
            raise RuntimeError(
                f"Missing ns-3 results "
                f"for silos: {sorted(missing)}"
            )

        # Communication phase time:
        # simultaneous transfers => maximum transfer/deadline time
        phase_time = max(
            result["time"]
            for result in results.values()
        )

        self.cumulative_network_time += (
            phase_time
        )

        successes = sum(
            result["success"]
            for result in results.values()
        )

        print(
            f"[Round {round_idx}] "
            f"TCP {phase}: "
            f"{successes}/{len(silo_ids)} successful "
            f"| phase time={phase_time:.3f}s",
            flush=True
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
                flush=True
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