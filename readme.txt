
--network_backend none
--network_backend wired_tcp
--network_backend wired_ndn
--network_backend wifi_tcp
--network_backend wifi_ndn

Server <-> Silo
1 Mbps
10 ms propagation delay
same model size
same TCP configuration
same start time
same queue/device configuration

// wired tcp
python main_sfl.py driving_gazebo --network_name gaia --network_backend wired_tcp --model FADNet --n_rounds 50 --bz_train 32 --bz_test 32 --device cuda --log_freq 40 --local_steps 1 --lr 0.001 --decay constant   2>&1 | tee ../gaia-sfl-tcp/sfl_wired_tcp_training.log
// wired ndn
python main_sfl.py driving_gazebo --network_name gaia --network_backend wired_ndn --model FADNet --n_rounds 50 --bz_train 32 --bz_test 32 --device cuda --log_freq 40 --local_steps 1 --lr 0.001 --decay constant   2>&1 | tee ../gaia-sfl-ndn/sfl_wired_ndn_training.log

python main_sfl.py driving_gazebo \
  --network_name gaia \
  --network_backend wired_ndn \
  --model FADNet \
  --n_rounds 2 \
  --bz_train 32 \
  --bz_test 32 \
  --device cuda \
  --log_freq 40 \
  --local_steps 1 \
  --lr 0.001 \
  --decay constant

./waf --run="gaia-sfl-ndn --phase=upload --round=1 --modelBytes=1367920 --deadline=15"
./waf --run="gaia-sfl-tcp --phase=download --round=1 --numSilos=20 --modelBytes=1367920 --deadline=15 --logFile=/home/hurair/ndnSIM/ns-3/scratch/gaia-sfl-tcp/logs/wired_tcp_network.csv"




python main_sfl.py driving_carla_multi \
  --data_path /mnt/d/CARLA_0.9.14/CarlaFLCAV/FLDatasetTool/raw_data \
  --carla_test_scenario record_2026_0506_1203 \
  --network_backend none --model FADNet --n_rounds 50 \
  --bz_train 32 --bz_test 32 --device cuda --local_steps 1 --lr 0.001 --decay constant



// Wifi NDN
cd /home/hurair/ndnSIM/ns-3/scratch/gaia-sfl-python
python main_sfl.py driving_gazebo --network_name gaia --network_backend wifi_ndn --model FADNet --n_rounds 10 --bz_train 32 --bz_test 32 --device cuda --log_freq 1 --local_steps 1 --lr 0.001 --decay constant 2>&1 | tee ../gaia-sfl-ndn-wifi/sfl_wifi_ndn_10round_test.log


// Wifi TCP
cd /home/hurair/ndnSIM/ns-3/scratch/gaia-sfl-python
python main_sfl.py driving_gazebo --network_name gaia --network_backend wifi_tcp --model FADNet --n_rounds 3000 --bz_train 32 --bz_test 32 --device cuda --log_freq 40 --local_steps 1 --lr 0.001 --decay constant 2>&1 | tee ../gaia-sfl-tcp-wifi/sfl_wifi_tcp_training.log
python main_sfl.py driving_gazebo --network_name gaia --network_backend wifi_tcp --model FADNet --n_rounds 3000 --bz_train 32 --bz_test 32 --device cuda --log_freq 40 --local_steps 1 --lr 0.001 --decay constant 2>&1 | tee ../gaia-sfl-tcp-wifi/sfl_wifi_tcp_training.log


NETWORK_BACKEND=wifi_tcp bash run.sh
