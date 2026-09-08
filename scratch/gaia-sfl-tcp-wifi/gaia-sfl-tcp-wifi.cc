#include "ns3/core-module.h"
#include "ns3/network-module.h"
#include "ns3/internet-module.h"
#include "ns3/point-to-point-module.h"
#include "ns3/wifi-module.h"
#include "ns3/mobility-module.h"
#include "ns3/applications-module.h"
#include "ns3/flow-monitor-module.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

using namespace ns3;

NS_LOG_COMPONENT_DEFINE("GaiaSflWifiTcp");

// -----------------------------------------------------
// Plain TCP/IP over WiFi, no NDN. Same physical topology as
// gaia-sfl-ndn-wifi.cc (RSU as a real AP, silos as WiFi
// stations, wired backhaul to the server) — reusing the
// realistic WiFi settings validated there — but transfers use
// ordinary TCP sockets instead of NDN Interest/Data. Standard
// ns-3 IP routing does real ARP-resolved unicast delivery with
// MAC-layer ACK/retry, which sidesteps the "every packet is an
// unacked L2 broadcast" limitation that ndnSIM's transport has
// on a shared WiFi medium.
// -----------------------------------------------------

struct TransferInfo
{
    uint32_t silo;
    uint32_t sender;
    uint32_t receiver;
    uint16_t port;

    uint64_t expectedBytes;

    double startTime;
    double deadlineTime;
    double finishTime;

    Ptr<PacketSink> sink;

    bool finished;
};

static std::vector<TransferInfo> g_transfers;

static void
CheckTransfers()
{
    double now = Simulator::Now().GetSeconds();

    for (auto& t : g_transfers)
    {
        if (t.finished)
        {
            continue;
        }

        if (now < t.startTime)
        {
            continue;
        }

        uint64_t rx = t.sink->GetTotalRx();

        if (rx >= t.expectedBytes)
        {
            t.finished = true;
            t.finishTime = now;
        }
    }

    Simulator::Schedule(
        MilliSeconds(5),
        &CheckTransfers
    );
}

static std::vector<uint32_t>
ParseSiloList(const std::string& text)
{
    std::vector<uint32_t> result;

    if (text.empty())
    {
        return result;
    }

    std::stringstream ss(text);
    std::string item;

    while (std::getline(ss, item, ','))
    {
        if (!item.empty())
        {
            result.push_back(
                static_cast<uint32_t>(std::stoul(item))
            );
        }
    }

    std::sort(result.begin(), result.end());

    return result;
}

int
main(int argc, char* argv[])
{
    // -----------------------------------------------------
    // Parameters
    // -----------------------------------------------------

    std::string phase = "download";

    uint32_t round = 1;

    uint32_t numSilos = 11;

    std::string activeSilos = "";

    uint64_t modelBytes =
        341980ULL * 4ULL;

    double deadline = 25.0;

    std::string logFile =
        "/home/hurair/ndnSIM/ns-3/"
        "scratch/gaia-sfl-tcp-wifi/"
        "wifi_tcp_network.csv";

    CommandLine cmd;

    cmd.AddValue(
        "phase",
        "download or upload",
        phase
    );

    cmd.AddValue(
        "round",
        "FL round number",
        round
    );

    cmd.AddValue(
        "numSilos",
        "Number of WiFi stations to create — should equal the "
        "number of active silos this call (not max(id)+1), so no "
        "idle stations get created for silo IDs that aren't part "
        "of this call.",
        numSilos
    );

    cmd.AddValue(
        "activeSilos",
        "Comma-separated list of the real silo IDs participating "
        "this run (e.g. \"5,6,7,9\"). Empty (default) means "
        "0..numSilos-1. Real IDs are only used for naming/NET_RESULT "
        "output — internally each active silo maps to its position "
        "in this (sorted) list for WiFi station/address indexing, so "
        "numSilos only ever needs to be len(activeSilos), never "
        "max(activeSilos)+1.",
        activeSilos
    );

    cmd.AddValue(
        "modelBytes",
        "Model size in bytes",
        modelBytes
    );

    cmd.AddValue(
        "deadline",
        "Communication deadline in seconds",
        deadline
    );

    cmd.AddValue(
        "logFile",
        "Network statistics CSV log file",
        logFile
    );

    cmd.Parse(argc, argv);

    if (
        phase != "download" &&
        phase != "upload"
    )
    {
        NS_FATAL_ERROR(
            "phase must be download or upload"
        );
    }

    if (numSilos == 0)
    {
        NS_FATAL_ERROR(
            "numSilos must be at least 1"
        );
    }

    // -----------------------------------------------------
    // numSilos silos + one central server + one RSU
    // -----------------------------------------------------

    NodeContainer silos;
    silos.Create(numSilos);

    Ptr<Node> server =
        CreateObject<Node>();

    Ptr<Node> rsu =
        CreateObject<Node>();

    // -----------------------------------------------------
    // Server <-> RSU <-> every silo. Same shape as
    // gaia-sfl-ndn-wifi.cc: the RSU is a real WiFi AP, every
    // silo is a station associated to it, all sharing one
    // channel; the server reaches the RSU over a wired backhaul.
    //
    //           Server
    //             |  (wired backhaul)
    //            RSU  == (AP)
    //          )) )) )) ))   (WiFi cell, shared medium)
    //        S0 S1 S2 ... S(numSilos-1)
    // -----------------------------------------------------

    InternetStackHelper internet;
    internet.Install(silos);
    internet.Install(server);
    internet.Install(rsu);

    const std::string backhaulRate =
        "100Mbps";

    const double backhaulDelayMs =
        2.0;

    NetDeviceContainer backhaulDevices;

    {
        PointToPointHelper p2p;

        p2p.SetDeviceAttribute(
            "DataRate",
            StringValue(backhaulRate)
        );

        p2p.SetChannelAttribute(
            "Delay",
            TimeValue(
                MilliSeconds(backhaulDelayMs)
            )
        );

        NodeContainer pair;

        pair.Add(server);
        pair.Add(rsu);

        backhaulDevices = p2p.Install(pair);
    }

    // Real-world WiFi realism knobs — identical to the validated
    // settings in gaia-sfl-ndn-wifi.cc: RTS/CTS, 802.11n (5GHz),
    // Minstrel rate control, 20 dBm TX power.
    Config::SetDefault(
        "ns3::WifiRemoteStationManager::RtsCtsThreshold",
        StringValue("500")
    );

    YansWifiChannelHelper wifiChannel;

    wifiChannel.SetPropagationDelay(
        "ns3::ConstantSpeedPropagationDelayModel"
    );

    wifiChannel.AddPropagationLoss(
        "ns3::FriisPropagationLossModel"
    );

    YansWifiPhyHelper wifiPhy;
    wifiPhy.SetChannel(wifiChannel.Create());

    wifiPhy.Set(
        "TxPowerStart",
        DoubleValue(20.0)
    );

    wifiPhy.Set(
        "TxPowerEnd",
        DoubleValue(20.0)
    );

    WifiHelper wifi;
    wifi.SetStandard(WIFI_STANDARD_80211n_5GHZ);
    // Confirmed diagnosis: Minstrel starts each station with no rate
    // history and has to guess, adjusting only from trial and error.
    // One station (silo 8) got permanently and deterministically
    // stuck failing at the PHY level — not just occasionally unlucky,
    // since giving it 4x more TCP SYN retries didn't help at all.
    // IdealWifiManager avoids this entirely: instead of probing and
    // guessing, it computes the appropriate rate directly from each
    // receiver's measured SNR, so there's no "bad initial guess" or
    // stuck-learning state to get trapped in.
    wifi.SetRemoteStationManager(
        // "ns3::IdealWifiManager"
        "ns3::MinstrelHtWifiManager"
    );

    Ssid ssid = Ssid("gaia-sfl-rsu");
    WifiMacHelper wifiMac;

    wifiMac.SetType(
        "ns3::StaWifiMac",
        "Ssid", SsidValue(ssid),
        "ActiveProbing", BooleanValue(false)
    );

    NetDeviceContainer staDevices =
        wifi.Install(wifiPhy, wifiMac, silos);

    wifiMac.SetType(
        "ns3::ApWifiMac",
        "Ssid", SsidValue(ssid)
    );

    NetDeviceContainer apDevice =
        wifi.Install(wifiPhy, wifiMac, rsu);

    // -----------------------------------------------------
    // Node positions — same ring layout as gaia-sfl-ndn-wifi.cc.
    // -----------------------------------------------------

    MobilityHelper mobility;

    Ptr<ListPositionAllocator> positionAlloc =
        CreateObject<ListPositionAllocator>();

    const double kPi = 3.14159265358979323846;
    const double radius = 50.0;

    positionAlloc->Add(
        Vector(0.0, 0.0, 0.0)
    );

    positionAlloc->Add(
        Vector(0.0, radius + 30.0, 0.0)
    );

    for (uint32_t i = 0; i < numSilos; ++i)
    {
        double angle =
            2.0 * kPi *
            static_cast<double>(i) /
            static_cast<double>(numSilos);

        positionAlloc->Add(
            Vector(
                radius * std::cos(angle),
                radius * std::sin(angle),
                0.0
            )
        );
    }

    mobility.SetPositionAllocator(positionAlloc);
    mobility.SetMobilityModel(
        "ns3::ConstantPositionMobilityModel"
    );

    NodeContainer positionedNodes;
    positionedNodes.Add(rsu);
    positionedNodes.Add(server);
    positionedNodes.Add(silos);

    mobility.Install(positionedNodes);

    // -----------------------------------------------------
    // IP addressing: backhaul link and the WiFi cell are two
    // separate subnets; the RSU forwards between them (ns-3
    // enables IP forwarding by default on multi-interface nodes).
    // -----------------------------------------------------

    Ipv4AddressHelper ipv4;

    ipv4.SetBase("10.1.1.0", "255.255.255.0");
    Ipv4InterfaceContainer backhaulInterfaces =
        ipv4.Assign(backhaulDevices);

    Ipv4Address serverAddress =
        backhaulInterfaces.GetAddress(0);

    NetDeviceContainer wifiDevices;
    wifiDevices.Add(staDevices);
    wifiDevices.Add(apDevice);

    ipv4.SetBase("10.2.0.0", "255.255.0.0");
    Ipv4InterfaceContainer wifiInterfaces =
        ipv4.Assign(wifiDevices);

    // wifiInterfaces[0..numSilos-1] are the silos (staDevices came
    // first), wifiInterfaces[numSilos] is the RSU (apDevice last).
    std::vector<Ipv4Address> siloAddress(numSilos);

    for (uint32_t i = 0; i < numSilos; ++i)
    {
        siloAddress[i] = wifiInterfaces.GetAddress(i);
    }

    Ipv4GlobalRoutingHelper::PopulateRoutingTables();

    // -----------------------------------------------------
    // Determine participating silos. Real silo IDs are only
    // used for naming/output — each active silo's position in
    // this sorted list (loopIdx) is what indexes into the
    // actually-created silos/siloAddress, since numSilos is
    // sized to len(selectedSilos), not max(selectedSilos)+1.
    // -----------------------------------------------------

    std::vector<uint32_t> selectedSilos;

    if (activeSilos.empty())
    {
        for (uint32_t i = 0; i < numSilos; ++i)
        {
            selectedSilos.push_back(i);
        }
    }
    else
    {
        selectedSilos = ParseSiloList(activeSilos);
    }

    // WiFi association (probe/auth/assoc with the AP) isn't instant,
    // unlike the wired scenario this was adapted from (P2P links have
    // no association step at all). All 11 stations start associating
    // around simulated time 0; if one takes a bit longer than others
    // to finish, TCP's SYN retries can exhaust before it's ready to
    // receive anything, permanently failing that connection every
    // round (observed: silo 8 consistently got 0/4 SYNs through).
    // Starting the transfer later gives every station room to finish
    // associating first.
    const double startTime = 3.0;

    uint16_t basePort = 9000;

    // -----------------------------------------------------
    // Schedule TCP transfers
    // -----------------------------------------------------

    for (
        uint32_t loopIdx = 0;
        loopIdx < selectedSilos.size();
        ++loopIdx
    )
    {
        uint32_t realSilo = selectedSilos[loopIdx];

        uint32_t senderId;
        uint32_t receiverId;

        Ptr<Node> senderNode;
        Ptr<Node> receiverNode;

        Ipv4Address receiverAddress;

        if (phase == "download")
        {
            // Server -> Silo
            senderId = numSilos;
            receiverId = realSilo;

            senderNode = server;
            receiverNode = silos.Get(loopIdx);

            receiverAddress = siloAddress[loopIdx];
        }
        else
        {
            // Silo -> Server
            senderId = realSilo;
            receiverId = numSilos;

            senderNode = silos.Get(loopIdx);
            receiverNode = server;

            receiverAddress = serverAddress;
        }

        // Every concurrent transfer terminates at a different
        // node in one direction and originates from a different
        // node in the other, but they can still share a listening
        // port on the server since each socket is identified by
        // the full (srcIP, srcPort, dstIP, dstPort) tuple — using
        // a unique port per transfer anyway keeps FlowMonitor's
        // per-flow classification unambiguous.
        uint16_t port =
            static_cast<uint16_t>(basePort + loopIdx);

        PacketSinkHelper sinkHelper(
            "ns3::TcpSocketFactory",
            InetSocketAddress(
                Ipv4Address::GetAny(),
                port
            )
        );

        ApplicationContainer sinkApps =
            sinkHelper.Install(receiverNode);

        sinkApps.Start(Seconds(0.5));
        sinkApps.Stop(Seconds(startTime + deadline + 0.5));

        Ptr<PacketSink> sink =
            DynamicCast<PacketSink>(sinkApps.Get(0));

        BulkSendHelper sender(
            "ns3::TcpSocketFactory",
            InetSocketAddress(receiverAddress, port)
        );

        sender.SetAttribute(
            "MaxBytes",
            UintegerValue(modelBytes)
        );

        ApplicationContainer senderApps =
            sender.Install(senderNode);

        senderApps.Start(Seconds(startTime));
        senderApps.Stop(Seconds(startTime + deadline));

        TransferInfo info;

        info.silo = realSilo;
        info.sender = senderId;
        info.receiver = receiverId;
        info.port = port;

        info.expectedBytes = modelBytes;

        info.startTime = startTime;
        info.deadlineTime = startTime + deadline;
        info.finishTime = -1.0;

        info.sink = sink;
        info.finished = false;

        g_transfers.push_back(info);
    }

    // -----------------------------------------------------
    // Log network statistics to CSV
    // -----------------------------------------------------

    FlowMonitorHelper flowHelper;
    Ptr<FlowMonitor> flowMonitor =
        flowHelper.InstallAll();

    Simulator::Schedule(
        Seconds(startTime),
        &CheckTransfers
    );

    Simulator::Stop(
        Seconds(startTime + deadline + 0.6)
    );

    Simulator::Run();

    flowMonitor->CheckForLostPackets();

    Ptr<Ipv4FlowClassifier> classifier =
        DynamicCast<Ipv4FlowClassifier>(
            flowHelper.GetClassifier()
        );

    std::map<FlowId, FlowMonitor::FlowStats> stats =
        flowMonitor->GetFlowStats();

    bool fileExists = false;

    {
        std::ifstream testFile(logFile);
        fileExists = testFile.good();
    }

    std::ofstream log(logFile, std::ios::app);

    if (!log.is_open())
    {
        NS_FATAL_ERROR(
            "Cannot open network log file: " << logFile
        );
    }

    if (!fileExists)
    {
        log
            << "round,"
            << "phase,"
            << "flow_id,"
            << "silo,"
            << "model_success,"
            << "src_ip,"
            << "dst_ip,"
            << "src_port,"
            << "dst_port,"
            << "tx_packets,"
            << "rx_packets,"
            << "lost_packets,"
            << "tx_bytes,"
            << "rx_bytes,"
            << "packet_loss_percent,"
            << "mean_delay_ms,"
            << "throughput_mbps"
            << std::endl;
    }

    for (const auto& entry : stats)
    {
        FlowId flowId = entry.first;
        const FlowMonitor::FlowStats& st = entry.second;

        Ipv4FlowClassifier::FiveTuple tuple =
            classifier->FindFlow(flowId);

        int32_t siloId = -1;
        bool modelSuccess = false;
        uint64_t sinkRx = 0;

        for (const auto& t : g_transfers)
        {
            if (
                tuple.destinationPort == t.port ||
                tuple.sourcePort == t.port
            )
            {
                siloId = static_cast<int32_t>(t.silo);

                sinkRx = t.sink->GetTotalRx();

                modelSuccess =
                    t.finished &&
                    sinkRx >= t.expectedBytes &&
                    t.finishTime <= t.deadlineTime;

                break;
            }
        }

        double packetLossPercent = 0.0;

        if (st.txPackets > 0)
        {
            packetLossPercent =
                100.0 *
                static_cast<double>(st.lostPackets) /
                static_cast<double>(st.txPackets);
        }

        double meanDelayMs = 0.0;

        if (st.rxPackets > 0)
        {
            meanDelayMs =
                st.delaySum.GetSeconds()
                / static_cast<double>(st.rxPackets)
                * 1000.0;
        }

        double throughputMbps = 0.0;

        if (
            st.rxPackets > 0 &&
            st.timeLastRxPacket > st.timeFirstTxPacket
        )
        {
            double duration =
                (st.timeLastRxPacket - st.timeFirstTxPacket)
                    .GetSeconds();

            throughputMbps =
                (static_cast<double>(st.rxBytes) * 8.0)
                / duration
                / 1000000.0;
        }

        log
            << round << ","
            << phase << ","
            << flowId << ","
            << siloId << ","
            << (modelSuccess ? 1 : 0) << ","
            << tuple.sourceAddress << ","
            << tuple.destinationAddress << ","
            << tuple.sourcePort << ","
            << tuple.destinationPort << ","
            << st.txPackets << ","
            << st.rxPackets << ","
            << st.lostPackets << ","
            << st.txBytes << ","
            << st.rxBytes << ","
            << std::fixed << std::setprecision(4)
            << packetLossPercent << ","
            << meanDelayMs << ","
            << throughputMbps
            << std::endl;
    }

    log.close();

    // -----------------------------------------------------
    // Machine-readable output for Python (same NET_RESULT
    // format ns3_tcp.py already parses from the wired scenario)
    // -----------------------------------------------------

    for (const auto& t : g_transfers)
    {
        uint64_t bytes = t.sink->GetTotalRx();

        bool success =
            t.finished &&
            bytes >= t.expectedBytes &&
            t.finishTime <= t.deadlineTime;

        double transferTime =
            success
                ? t.finishTime - t.startTime
                : deadline;

        std::cout
            << "NET_RESULT"
            << " round=" << round
            << " phase=" << phase
            << " silo=" << t.silo
            << " sender=" << t.sender
            << " receiver=" << t.receiver
            << " success=" << (success ? 1 : 0)
            << " time=" << transferTime
            << " bytes=" << bytes
            << std::endl;
    }

    Simulator::Destroy();

    return 0;
}
