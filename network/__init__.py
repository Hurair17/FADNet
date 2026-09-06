from .base import NetworkBackend
from .no_network import NoNetworkBackend

def __init__(
    self,
    args,
    network_backend
):
    self.args = args
    self.network = network_backend