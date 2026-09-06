import torch

from utils.args import parse_args
from sfl import ServerFederatedLearning
from network.no_network import NoNetworkBackend
from network.ns3_tcp import Ns3TcpBackend
from network.ns3_ndn import Ns3NdnBackend


if __name__ == "__main__":

    torch.manual_seed(1204)

    torch.backends.cudnn.deterministic = True
    torch.backends.cudnn.benchmark = False

    args = parse_args()
    MODEL_BYTES = 1367920
    if args.network_backend == "none":
        backend = NoNetworkBackend(
            model_bytes=MODEL_BYTES
        )
    elif args.network_backend == "wired_tcp":
        backend = Ns3TcpBackend(
            model_bytes=MODEL_BYTES,
            deadline=20.0,
        )
    elif args.network_backend == "wired_ndn":
        backend = Ns3NdnBackend(
            model_bytes=MODEL_BYTES,
            deadline=25.0,
        )
    elif args.network_backend == "wifi_ndn":
        backend = Ns3NdnBackend(
            model_bytes=MODEL_BYTES,
            deadline=90.0,
            program="gaia-sfl-ndn-wifi",
            trace_prefix="wifi_ndn",
            # Download (single broadcaster) works fine with the default
            # ConsumerWindow. Upload (11 concurrent broadcasters) needs
            # ConsumerCbr's fixed-rate pacing instead — ConsumerWindow's
            # AIMD hard-reset-on-timeout causes a starvation collapse
            # under multi-station WiFi contention; see gaia-sfl-ndn-wifi.cc.
            upload_extra_args="--consumerType=cbr --cbrFrequency=20",
        )
    else:

        raise ValueError(
            f"Unknown network backend: "
            f"{args.network_backend}"
        )

    sfl = ServerFederatedLearning(
        args,
        network_backend=backend
    )



    for _ in range(args.n_rounds):
        sfl.run_round()
        if (
            sfl.round_idx % args.log_freq == 0
            or
            sfl.round_idx == args.n_rounds
        ):
            sfl.evaluate()