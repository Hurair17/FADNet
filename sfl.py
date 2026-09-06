import os
import re
import time

import torch
import torchvision.transforms as transforms

from utils.utils import get_iterator, get_model, EXTENSIONS
from dataset.carla_federated import (
    build_carla_train_loaders,
    build_carla_combined_loader,
    find_vehicle_dirs,
)


def _bottom_crop(image, crop_size=(200, 200)):
    """Crop centered horizontally, anchored to the bottom vertically
    (keeps the road, drops the sky) — matches loaders/complex_driving.py."""
    width, height = image.size
    crop_w, crop_h = crop_size
    left = (width - crop_w) // 2
    top = height - crop_h
    return image.crop((left, top, left + crop_w, top + crop_h))


# Matches the (320x240 resize -> 200x200 bottom crop -> grayscale ->
# [-1, 1]) pipeline that loaders/complex_driving.py applies for the other
# driving datasets, so FADNet sees the same input shape/range either way.
CARLA_TRANSFORM = transforms.Compose([
    transforms.Grayscale(num_output_channels=1),
    transforms.Resize((240, 320)),
    transforms.Lambda(_bottom_crop),
    transforms.ToTensor(),
    transforms.Lambda(lambda x: x * 2.0 - 1.0),
])


class ServerFederatedLearning:

    def __init__(self, args, network_backend):
        self.args = args
        self.network = network_backend
        self.device = args.device

        self.round_idx = 0

        self.local_steps = args.local_steps
        self.fit_by_epoch = args.fit_by_epoch

        if args.experiment == "driving_carla_multi":
            self._init_carla_multi(args)
        else:
            self._init_npz_dataset(args)

        # Client models — one per silo (n_workers is dataset-dependent:
        # 11 for driving_gazebo, len(scenarios) * vehicles for driving_carla_multi)
        self.workers_models = [
            get_model(
                args.experiment,
                args.model,
                self.device,
                optimizer_name=args.optimizer,
                lr_scheduler=args.decay,
                initial_lr=args.lr,
                epoch_size=self.epoch_size
            )
            for _ in range(self.n_workers)
        ]

        # One central server/global model
        self.global_model = get_model(
            args.experiment,
            args.model,
            self.device,
            epoch_size=self.epoch_size
        )

    def _init_carla_multi(self, args):

        if not args.data_path:
            raise ValueError(
                "--data_path is required for driving_carla_multi "
                "(the directory containing the scenario subfolders, e.g. "
                "the WSL path to .../CarlaFLCAV/FLDatasetTool/raw_data)"
            )

        root_dir = args.data_path

        # Only folders that actually contain "vehicle.*_<id>" recordings
        # count as scenarios; this skips unrelated tool/debug folders that
        # can sit alongside real recordings under the same raw_data root.
        scenario_dirs = sorted(
            name
            for name in os.listdir(root_dir)
            if os.path.isdir(os.path.join(root_dir, name))
            and find_vehicle_dirs(os.path.join(root_dir, name))
        )

        if not scenario_dirs:
            raise ValueError(
                f"No CARLA scenario folders found under {root_dir}"
            )

        test_scenario = args.carla_test_scenario or scenario_dirs[-1]

        if test_scenario not in scenario_dirs:
            raise ValueError(
                f"--carla_test_scenario '{test_scenario}' not found under "
                f"{root_dir}; available: {scenario_dirs}"
            )

        train_scenarios = [
            scenario
            for scenario in scenario_dirs
            if scenario != test_scenario
        ]

        print(
            f"[CARLA] train scenarios: {train_scenarios} | "
            f"held-out test scenario: {test_scenario}"
        )

        # One DataLoader (= one FL silo) per vehicle across the train scenarios
        self.workers_iterators, sample_counts, self.silo_info = \
            build_carla_train_loaders(
                root_dir,
                train_scenarios,
                CARLA_TRANSFORM,
                batch_size=args.bz_train
            )

        self.n_workers = len(self.workers_iterators)

        if self.n_workers == 0:
            raise ValueError(
                f"No CARLA vehicles found under {root_dir} "
                f"for scenarios {train_scenarios}"
            )

        print(f"[CARLA] {self.n_workers} silos")

        # Global train/test iterators (pooled across all vehicles)
        self.train_iterator = build_carla_combined_loader(
            root_dir,
            train_scenarios,
            CARLA_TRANSFORM,
            batch_size=args.bz_test
        )

        self.test_iterator = build_carla_combined_loader(
            root_dir,
            [test_scenario],
            CARLA_TRANSFORM,
            batch_size=args.bz_test
        )

        self.epoch_size = int(
            sum(len(loader) for loader in self.workers_iterators)
            / self.n_workers
        )

    def _init_npz_dataset(self, args):

        self.train_dir = os.path.join(
            "data",
            args.experiment,
            args.network_name,
            "train"
        )

        self.test_dir = os.path.join(
            "data",
            args.experiment,
            args.network_name,
            "test"
        )

        extension = EXTENSIONS["driving"]

        self.train_path = os.path.join(
            self.train_dir,
            "train" + extension
        )

        self.test_path = os.path.join(
            self.test_dir,
            "test" + extension
        )

        # Global train/test iterators
        self.train_iterator = get_iterator(
            args.experiment,
            self.train_path,
            self.device,
            args.bz_test
        )

        self.test_iterator = get_iterator(
            args.experiment,
            self.test_path,
            self.device,
            args.bz_test
        )

        # Discover per-worker files (e.g. "0.npz".."10.npz") instead of
        # assuming a fixed count, since different network topologies
        # (gaia, amazon_us, ...) ship a different number of workers.
        worker_pattern = re.compile(
            r"^(\d+)" + re.escape(extension) + r"$"
        )

        worker_ids = sorted(
            int(match.group(1))
            for match in (
                worker_pattern.match(name)
                for name in os.listdir(self.train_dir)
            )
            if match
        )

        self.n_workers = len(worker_ids)

        if self.n_workers == 0:
            raise ValueError(
                f"No per-worker {extension} files found under "
                f"{self.train_dir}"
            )

        # Local worker datasets
        self.workers_iterators = []

        train_data_size = 0

        for worker_id in worker_ids:

            path = os.path.join(
                self.train_dir,
                str(worker_id) + extension
            )

            iterator = get_iterator(
                args.experiment,
                path,
                self.device,
                args.bz_train
            )

            self.workers_iterators.append(
                iterator
            )

            train_data_size += len(iterator)

        self.epoch_size = int(
            train_data_size / self.n_workers
        )

    def broadcast_global_model(self):

        global_state = self.global_model.net.state_dict()

        for worker in self.workers_models:
            worker.net.load_state_dict(
                global_state
            )

    def local_training(self):

        for worker_id, model in enumerate(
            self.workers_models
        ):

            model.net.to(self.device)

            if self.fit_by_epoch:

                model.fit_iterator(
                    train_iterator=
                        self.workers_iterators[worker_id],
                    n_epochs=self.local_steps,
                    verbose=0
                )

            else:

                model.fit_batches(
                    iterator=
                        self.workers_iterators[worker_id],
                    n_steps=self.local_steps
                )

    # def aggregate(self):

    #     with torch.no_grad():

    #         for param_idx, global_param in enumerate(
    #             self.global_model.net.parameters()
    #         ):

    #             global_param.data.zero_()

    #             for worker_model in self.workers_models:

    #                 worker_param = list(
    #                     worker_model.net.parameters()
    #                 )[param_idx].data

    #                 global_param.data += (
    #                     worker_param /
    #                     self.n_workers
    #                 )

    def aggregate(self, received_silos):

        if len(received_silos) == 0:
            print(
                "No local models received. "
                "Global model remains unchanged."
            )
            return

        with torch.no_grad():

            for param_idx, global_param in enumerate(
                self.global_model.net.parameters()
            ):

                global_param.data.zero_()

                for silo_id in received_silos:

                    local_param = list(
                        self.workers_models[
                            silo_id
                        ].net.parameters()
                    )[param_idx].data

                    global_param.data += (
                        local_param /
                        len(received_silos)
                    )

    def evaluate(self):

        print(">>>>>>>>>> Evaluating SFL")

        print("\t - train set")

        train_loss, train_rmse = \
            self.global_model.evaluate_iterator(
                self.train_iterator
            )

        print("\t - test set")

        test_loss, test_rmse = \
            self.global_model.evaluate_iterator(
                self.test_iterator
            )

        print(
            f"Round: {self.round_idx} "
            f"| Train Loss: {train_loss:.5f} "
            f"| Train RMSE: {train_rmse:.5f}"
        )

        print(
            f"Test Loss: {test_loss:.5f} "
            f"| Test RMSE: {test_rmse:.5f}"
        )

    # def run_round(self):

    #     round_number = self.round_idx + 1

    #     print(
    #         f"\n========== SFL ROUND {round_number} =========="
    #     )

    #     # -----------------------------------------
    #     # 1. Server -> clients
    #     # -----------------------------------------

    #     silo_ids = list(range(self.n_workers))

    #     download_results = self.network.download(
    #         round_number,
    #         silo_ids
    #     )

    #     active_silos = []

    #     for silo_id in silo_ids:

    #         if download_results[silo_id]["success"]:

    #             self.workers_models[
    #                 silo_id
    #             ].net.load_state_dict(
    #                 self.global_model.net.state_dict()
    #             )

    #             active_silos.append(silo_id)

    #         else:

    #             print(
    #                 f"[Round {round_number}] "
    #                 f"Silo {silo_id}: DOWNLOAD FAILED"
    #             )

    #     # -----------------------------------------
    #     # 2. Local training
    #     # -----------------------------------------

    #     for silo_id in active_silos:

    #         print(
    #             f"[Round {round_number}] "
    #             f"Silo {silo_id}: training"
    #         )

    #         self.workers_models[
    #             silo_id
    #         ].fit_batches(
    #             iterator=self.workers_iterators[silo_id],
    #             n_steps=self.local_steps
    #         )

    #     # -----------------------------------------
    #     # 3. Clients -> server
    #     # -----------------------------------------

    #     upload_results = self.network.upload(
    #         round_number,
    #         active_silos
    #     )

    #     received_silos = [
    #         silo_id
    #         for silo_id in active_silos
    #         if upload_results[silo_id]["success"]
    #     ]

    #     # -----------------------------------------
    #     # 4. Server aggregation
    #     # -----------------------------------------

    #     self.aggregate(
    #         received_silos
    #     )

    #     self.round_idx += 1

    def run_round(self):

        round_number = self.round_idx + 1

        print(
            f"\n========== SFL ROUND {round_number} =========="
        )

        silo_ids = list(range(self.n_workers))

        # 1. Server -> silos
        download_results = self.network.download(
            round_number,
            silo_ids
        )

        active_silos = []

        for silo_id in silo_ids:

            if download_results[silo_id]["success"]:

                self.workers_models[
                    silo_id
                ].net.load_state_dict(
                    self.global_model.net.state_dict()
                )

                active_silos.append(silo_id)

        # 2. Local training
        for silo_id in active_silos:

            print(
                f"[Round {round_number}] "
                f"Silo {silo_id}: training"
            )

            self.workers_models[
                silo_id
            ].fit_batches(
                iterator=self.workers_iterators[silo_id],
                n_steps=self.local_steps
            )

        # 3. Silos -> server
        upload_results = self.network.upload(
            round_number,
            active_silos
        )

        received_silos = [
            silo_id
            for silo_id in active_silos
            if upload_results[silo_id]["success"]
        ]

        print(
            f"[Round {round_number}] "
            f"Server received "
            f"{len(received_silos)}/{self.n_workers} models"
        )

        # 4. FedAvg
        self.aggregate(received_silos)

        print(
            f"[Round {round_number}] "
            "Server aggregation complete"
        )

        self.round_idx += 1

