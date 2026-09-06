from abc import ABC, abstractmethod


class NetworkBackend(ABC):

    @abstractmethod
    def download(self, round_idx, silo_ids):
        pass

    @abstractmethod
    def upload(self, round_idx, silo_ids):
        pass