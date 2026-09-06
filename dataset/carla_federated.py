import os
import re

from torch.utils.data import ConcatDataset, DataLoader

from .carla import CarlaSteeringDataset

# Numbered "vehicle.*" subfolders only (e.g. "vehicle.tesla.model3_13");
# this skips the unsuffixed ego/master recording in each scenario folder,
# as well as unrelated debug/tool folders (e.g. "others.world_0") that can
# end up alongside real scenario recordings under the same raw_data root.
_VEHICLE_DIR_RE = re.compile(r"^vehicle\..*_(\d+)$")


def find_vehicle_dirs(scenario_dir):
    return sorted(
        name
        for name in os.listdir(scenario_dir)
        if os.path.isdir(os.path.join(scenario_dir, name))
        and _VEHICLE_DIR_RE.match(name)
    )


def build_carla_train_loaders(
    root_dir,
    scenarios,
    transform,
    batch_size=32,
):
    """
    One DataLoader (= one FL silo) per vehicle, across all given scenarios.
    The number of silos is dynamic: len(scenarios) * vehicles-per-scenario.
    """

    loaders = []
    sample_counts = []
    silo_info = []

    silo_id = 0

    for scenario in scenarios:

        scenario_dir = os.path.join(
            root_dir,
            scenario
        )

        for vehicle in find_vehicle_dirs(scenario_dir):

            vehicle_dir = os.path.join(
                scenario_dir,
                vehicle
            )

            dataset = CarlaSteeringDataset(
                vehicle_dir=vehicle_dir,
                transform=transform
            )

            loader = DataLoader(
                dataset,
                batch_size=batch_size,
                shuffle=True,
                num_workers=4,
                pin_memory=True
            )

            loaders.append(loader)

            sample_counts.append(
                len(dataset)
            )

            silo_info.append({
                "silo": silo_id,
                "scenario": scenario,
                "vehicle": vehicle,
                "samples": len(dataset)
            })

            print(
                f"Silo {silo_id}: "
                f"{scenario}/{vehicle} "
                f"({len(dataset)} samples)"
            )

            silo_id += 1

    return loaders, sample_counts, silo_info


def build_carla_combined_loader(
    root_dir,
    scenarios,
    transform,
    batch_size=32,
    shuffle=False,
):
    """
    Single DataLoader pooling every vehicle across the given scenarios;
    used for global train/test evaluation (not local silo training).
    """

    datasets = []

    for scenario in scenarios:

        scenario_dir = os.path.join(
            root_dir,
            scenario
        )

        for vehicle in find_vehicle_dirs(scenario_dir):

            vehicle_dir = os.path.join(
                scenario_dir,
                vehicle
            )

            datasets.append(
                CarlaSteeringDataset(
                    vehicle_dir=vehicle_dir,
                    transform=transform
                )
            )

    combined_dataset = ConcatDataset(
        datasets
    )

    loader = DataLoader(
        combined_dataset,
        batch_size=batch_size,
        shuffle=shuffle,
        num_workers=4,
        pin_memory=True
    )

    print(
        f"CARLA combined samples ({', '.join(scenarios)}): "
        f"{len(combined_dataset)}"
    )

    return loader
