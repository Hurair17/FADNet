#!/usr/bin/env bash

set -euo pipefail

echo "=================================================="
echo " FADNet / SFL / Wi-Fi NDN H200 Run"
echo "=================================================="

# ==================================================
# Repository root
# ==================================================

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$ROOT_DIR"

echo "[INFO] Repository root: $ROOT_DIR"

PYTHON="${PYTHON:-python3}"

# ==================================================
# Configuration
# ==================================================

EXPERIMENT="${EXPERIMENT:-driving_gazebo}"
MODEL="${MODEL:-FADNet}"

N_ROUNDS="${N_ROUNDS:-3000}"
BATCH_TRAIN="${BATCH_TRAIN:-32}"
BATCH_TEST="${BATCH_TEST:-32}"
LOCAL_STEPS="${LOCAL_STEPS:-1}"
LR="${LR:-0.001}"
LOG_FREQ="${LOG_FREQ:-40}"

NETWORK_NAME="${NETWORK_NAME:-gaia}"

# Change to wifi_tcp if required.
NETWORK_BACKEND="${NETWORK_BACKEND:-wifi_ndn}"

# ns-3 / ndnSIM installation location
NDNSIM_ROOT="${NDNSIM_ROOT:-$ROOT_DIR/ndnSIM}"
NS3_DIR="$NDNSIM_ROOT/ns-3"

# GAIA dataset
DATA_ROOT="$ROOT_DIR/data"
DATA_DIR="$DATA_ROOT/driving_gazebo"
DATA_ZIP="$DATA_ROOT/driving_gazebo.zip"

GAIA_URL="https://huggingface.co/datasets/aiozai/DFL_framework/resolve/main/driving_gazebo.zip"

# Output
OUTPUT_ROOT="${OUTPUT_ROOT:-$ROOT_DIR/output}"
mkdir -p "$OUTPUT_ROOT"

TIMESTAMP="$(date +"%Y%m%d_%H%M%S")"
LOG_FILE="$OUTPUT_ROOT/fadnet_wifi_ndn_${TIMESTAMP}.log"

# Make ns-3 path visible to Python network backends.
export NS3_DIR
export NDNSIM_ROOT

echo "[INFO] NS3_DIR=$NS3_DIR"
echo "[INFO] NETWORK_BACKEND=$NETWORK_BACKEND"

# ==================================================
# GPU check
# ==================================================

echo
echo "=================================================="
echo " GPU information"
echo "=================================================="

if command -v nvidia-smi >/dev/null 2>&1; then
    nvidia-smi
else
    echo "[ERROR] nvidia-smi not found."
    exit 1
fi

# ==================================================
# System dependencies
# ==================================================

echo
echo "=================================================="
echo " Installing system dependencies"
echo "=================================================="

if command -v apt-get >/dev/null 2>&1; then

    if [ "$(id -u)" -eq 0 ]; then
        APT="apt-get"
    elif command -v sudo >/dev/null 2>&1; then
        APT="sudo apt-get"
    else
        echo "[ERROR] apt-get requires root/sudo access."
        exit 1
    fi

    $APT update

    $APT install -y \
        build-essential \
        gcc \
        g++ \
        git \
        pkg-config \
        cmake \
        ninja-build \
        wget \
        curl \
        unzip \
        libsqlite3-dev \
        libboost-all-dev \
        libssl-dev \
        python3 \
        python3-dev \
        python3-pip \
        python3-setuptools \
        castxml

else

    echo "[WARNING] apt-get not available."
    echo "[WARNING] Assuming required system packages already exist."

fi

# ==================================================
# Python dependencies
# ==================================================

echo
echo "=================================================="
echo " Python dependencies"
echo "=================================================="

$PYTHON -m pip install --upgrade pip setuptools wheel

if [ -f "$ROOT_DIR/requirements.txt" ]; then

    echo "[INFO] Installing requirements.txt"

    $PYTHON -m pip install \
        -r "$ROOT_DIR/requirements.txt"

else

    echo "[INFO] requirements.txt not found."
    echo "[INFO] Installing common FADNet packages."

    $PYTHON -m pip install \
        numpy \
        pandas \
        scipy \
        scikit-learn \
        pillow \
        matplotlib \
        tqdm \
        openpyxl

fi

# ==================================================
# CUDA / PyTorch check
# ==================================================

echo
echo "=================================================="
echo " CUDA / PyTorch check"
echo "=================================================="

$PYTHON - <<'PY'
import sys

try:
    import torch
except ImportError:
    print("[ERROR] PyTorch is not installed.")
    sys.exit(1)

print("PyTorch :", torch.__version__)
print("CUDA    :", torch.version.cuda)
print("Available:", torch.cuda.is_available())

if not torch.cuda.is_available():
    raise RuntimeError("CUDA is unavailable.")

print("GPU count:", torch.cuda.device_count())

for i in range(torch.cuda.device_count()):
    print(
        f"GPU {i}:",
        torch.cuda.get_device_name(i)
    )

x = torch.randn(
    1024,
    1024,
    device="cuda"
)

y = x @ x

print("CUDA test: SUCCESS")
PY

# ==================================================
# Download GAIA dataset
# ==================================================

echo
echo "=================================================="
echo " GAIA / Gazebo dataset"
echo "=================================================="

mkdir -p "$DATA_ROOT"

if [ ! -d "$DATA_DIR" ] || \
   [ -z "$(ls -A "$DATA_DIR" 2>/dev/null || true)" ]; then

    echo "[INFO] GAIA dataset not found."
    echo "[INFO] Downloading..."

    if command -v wget >/dev/null 2>&1; then

        wget -c \
            "$GAIA_URL" \
            -O "$DATA_ZIP"

    else

        curl \
            -L \
            --retry 5 \
            --retry-delay 5 \
            "$GAIA_URL" \
            -o "$DATA_ZIP"

    fi

    echo "[INFO] Extracting..."

    unzip -q \
        "$DATA_ZIP" \
        -d "$DATA_ROOT"

else

    echo "[INFO] Existing dataset found:"
    echo "       $DATA_DIR"

fi

if [ ! -d "$DATA_DIR" ]; then
    echo "[ERROR] Dataset extraction failed."
    exit 1
fi

echo "[INFO] Dataset size:"
du -sh "$DATA_DIR"

echo "[INFO] Example files:"
find "$DATA_DIR" \
    -maxdepth 2 \
    -type f \
    | head -20 || true

# ==================================================
# Download / build ndnSIM
# ==================================================

echo
echo "=================================================="
echo " ns-3 / ndnSIM setup"
echo "=================================================="

mkdir -p "$NDNSIM_ROOT"

if [ ! -d "$NS3_DIR" ]; then

    echo "[INFO] Cloning ndnSIM ns-3 tree..."

    git clone \
        https://github.com/named-data-ndnSIM/ns-3-dev.git \
        "$NS3_DIR"

fi

if [ ! -d "$NS3_DIR/src/ndnSIM" ]; then

    echo "[INFO] Cloning ndnSIM..."

    git clone \
        --recursive \
        https://github.com/named-data-ndnSIM/ndnSIM.git \
        "$NS3_DIR/src/ndnSIM"

else

    echo "[INFO] ndnSIM already exists."

    (
        cd "$NS3_DIR/src/ndnSIM"

        git submodule update \
            --init \
            --recursive
    )

fi

# ==================================================
# Build ns-3 / ndnSIM
# ==================================================

cd "$NS3_DIR"

echo
echo "[INFO] Configuring ndnSIM..."

./waf configure \
    -d optimized \
    --disable-python \
    --enable-examples

echo
echo "[INFO] Building ndnSIM..."

./waf

echo "[INFO] ndnSIM build complete."

# ==================================================
# Copy project Wi-Fi scenarios into ns-3 scratch
# ==================================================

echo
echo "=================================================="
echo " Installing project ns-3 scenarios"
echo "=================================================="

#
# Adjust these paths if your repository stores the
# scenarios somewhere else.
#

PROJECT_SCENARIOS=(
    "gaia-sfl-ndn"
    "gaia-sfl-ndn-wifi"
    "gaia-sfl-tcp"
)

for scenario in "${PROJECT_SCENARIOS[@]}"; do

    project_scenario="$ROOT_DIR/scratch/$scenario"

    if [ -d "$project_scenario" ]; then

        echo "[INFO] Copying $scenario scenario..."

        rm -rf \
            "$NS3_DIR/scratch/$scenario"

        cp -r \
            "$project_scenario" \
            "$NS3_DIR/scratch/"

    else

        echo "[WARNING] Scenario not found, skipping: $project_scenario"

    fi

done

# Rebuild scratch programs

cd "$NS3_DIR"

echo "[INFO] Building project ns-3 scenarios..."

./waf

# ==================================================
# Test Wi-Fi NDN simulator
# ==================================================

if [ "$NETWORK_BACKEND" = "wifi_ndn" ]; then

    echo
    echo "=================================================="
    echo " Testing Wi-Fi NDN scenario"
    echo "=================================================="

    ./waf --run="gaia-sfl-ndn-wifi \
--phase=download \
--round=1 \
--numSilos=11 \
--modelBytes=1367920 \
--deadline=60"

fi

# ==================================================
# Locate main_sfl.py
# ==================================================

cd "$ROOT_DIR"

MAIN_FILE=""

if [ -f "$ROOT_DIR/main_sfl.py" ]; then

    MAIN_FILE="$ROOT_DIR/main_sfl.py"

elif [ -f "$ROOT_DIR/python/main_sfl.py" ]; then

    MAIN_FILE="$ROOT_DIR/python/main_sfl.py"

else

    MAIN_FILE="$(
        find "$ROOT_DIR" \
            -name main_sfl.py \
            -type f \
            | head -1 || true
    )"

fi

if [ -z "$MAIN_FILE" ]; then

    echo "[ERROR] main_sfl.py not found."
    exit 1

fi

MAIN_DIR="$(dirname "$MAIN_FILE")"

echo "[INFO] main_sfl.py:"
echo "       $MAIN_FILE"

cd "$MAIN_DIR"

# ==================================================
# Display experiment settings
# ==================================================

echo
echo "=================================================="
echo " Experiment configuration"
echo "=================================================="

echo "Experiment       : $EXPERIMENT"
echo "Model            : $MODEL"
echo "Rounds           : $N_ROUNDS"
echo "Batch train      : $BATCH_TRAIN"
echo "Batch test       : $BATCH_TEST"
echo "Local steps      : $LOCAL_STEPS"
echo "Learning rate    : $LR"
echo "Network          : $NETWORK_NAME"
echo "Backend          : $NETWORK_BACKEND"
echo "ns-3             : $NS3_DIR"
echo "Dataset          : $DATA_DIR"
echo "GPU              : cuda"

# ==================================================
# Run FADNet SFL
# ==================================================

echo
echo "=================================================="
echo " Starting FADNet"
echo "=================================================="

$PYTHON main_sfl.py "$EXPERIMENT" \
    --network_name "$NETWORK_NAME" \
    --network_backend "$NETWORK_BACKEND" \
    --model "$MODEL" \
    --n_rounds "$N_ROUNDS" \
    --bz_train "$BATCH_TRAIN" \
    --bz_test "$BATCH_TEST" \
    --device cuda \
    --log_freq "$LOG_FREQ" \
    --local_steps "$LOCAL_STEPS" \
    --lr "$LR" \
    --decay constant \
    2>&1 | tee "$LOG_FILE"

echo
echo "=================================================="
echo " Experiment finished"
echo "=================================================="

echo "[INFO] Main log:"
echo "       $LOG_FILE"

echo
echo "[INFO] Output files:"

find "$OUTPUT_ROOT" \
    -maxdepth 4 \
    -type f \
    -print \
    2>/dev/null || true

echo
echo "[INFO] ns-3 trace files:"

find "$NS3_DIR/scratch" \
    -type f \
    \( \
        -name "*.txt" \
        -o \
        -name "*.csv" \
    \) \
    -print \
    2>/dev/null || true