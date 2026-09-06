from .base import NetworkBackend


class NoNetworkBackend(NetworkBackend):

    def __init__(self, model_bytes):
        self.model_bytes = model_bytes

    def download(self, round_idx, silo_ids):
        return {
            silo_id: {
                "success": True,
                "time": 0.0,
                "bytes": self.model_bytes,
            }
            for silo_id in silo_ids
        }

    def upload(self, round_idx, silo_ids):
        return {
            silo_id: {
                "success": True,
                "time": 0.0,
                "bytes": self.model_bytes,
            }
            for silo_id in silo_ids
        }