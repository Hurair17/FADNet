import os

import pandas as pd
import torch
from PIL import Image
from torch.utils.data import Dataset


class CarlaSteeringDataset(Dataset):
    """
    One CARLA vehicle recording: a `vehicle_status.csv` log (with `frame` and
    `steer` columns) plus a sibling image folder of frame-numbered PNGs,
    e.g. vehicle_dir/image_2/0000010122.png for frame 10122.
    """

    def __init__(
        self,
        vehicle_dir,
        transform=None,
        status_name="vehicle_status.csv",
        image_folder="image_2",
        frame_column="frame",
        steering_column="steer",
        image_extension=".png",
        frame_digits=10,
    ):
        self.vehicle_dir = vehicle_dir
        self.transform = transform

        status_path = os.path.join(
            vehicle_dir,
            status_name
        )

        image_dir = os.path.join(
            vehicle_dir,
            image_folder
        )

        df = pd.read_csv(status_path)

        self.samples = []

        for _, row in df.iterrows():

            frame = int(
                row[frame_column]
            )

            steering = float(
                row[steering_column]
            )

            image_name = (
                f"{frame:0{frame_digits}d}{image_extension}"
            )

            image_path = os.path.join(
                image_dir,
                image_name
            )

            if not os.path.isfile(image_path):
                continue

            self.samples.append(
                (image_path, steering)
            )

        print(
            f"Loaded {len(self.samples)} samples "
            f"from {vehicle_dir}"
        )

    def __len__(self):
        return len(self.samples)

    def __getitem__(self, idx):

        image_path, steering = self.samples[idx]

        image = Image.open(
            image_path
        ).convert("RGB")

        if self.transform is not None:
            image = self.transform(image)

        steering = torch.tensor(
            steering,
            dtype=torch.float32
        )

        return image, steering
