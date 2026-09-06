#include "ns3/core-module.h"
#include "ns3/network-module.h"
#include "ns3/internet-module.h"
#include "ns3/point-to-point-module.h"
#include "ns3/applications-module.h"

#include "ns3/flow-monitor-module.h"
#include <fstream>
#include <iomanip>

#include <algorithm>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

using namespace ns3;

NS_LOG_COMPONENT_DEFINE("GaiaSflWiredTcp");

struct GaiaLink
{
    uint32_t a;
    uint32_t b;
    double delayMs;
    std::string rate;
};

struct TransferInfo
{
    uint32_t silo;
    uint32_t sender;
    uint32_t receiver;

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
                static_cast<uint32_t>(
                    std::stoul(item)
                )
            );
        }
    }

    return result;
}

int
main(int argc, char* argv[])
{
    std::string phase = "download";
    std::string activeSilos = "";

    uint32_t round = 1;

    uint32_t numSilos = 11;

    uint64_t modelBytes =
        341980ULL * 4ULL;

    double deadline = 15.0;
    std::string logFile =
    "/home/hurair/ndnSIM/ns-3/scratch/gaia-sfl-tcp/logs/"
    "wired_tcp_network.csv";

    CommandLine cmd;

    cmd.AddValue(
        "phase",
        "download or upload",
        phase
    );
    cmd.AddValue(
        "logFile",
        "Network statistics log file",
        logFile
    );

    cmd.AddValue(
        "round",
        "FL round number",
        round
    );

    cmd.AddValue(
        "numSilos",
        "Number of FL silos in the star topology",
        numSilos
    );

    cmd.AddValue(
        "activeSilos",
        "Comma-separated silo IDs",
        activeSilos
    );

    cmd.AddValue(
        "modelBytes",
        "Model size in bytes",
        modelBytes
    );

    cmd.AddValue(
        "deadline",
        "Communication deadline",
        deadline
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

    if (
        numSilos == 0 ||
        numSilos > 254
    )
    {
        // Each silo gets its own /24 in the third octet
        // (10.1.<1..numSilos>.0), so it must fit in one byte.
        NS_FATAL_ERROR(
            "numSilos must be between 1 and 254"
        );
    }

    // -----------------------------------------------------
    // numSilos FL silos + 1 central FL server
    // -----------------------------------------------------

    NodeContainer silos;
    silos.Create(numSilos);

    Ptr<Node> server =
        CreateObject<Node>();

    const uint32_t SERVER_ID = numSilos;

    // -----------------------------------------------------
    // TCP/IP
    // -----------------------------------------------------

    InternetStackHelper internet;

    internet.Install(silos);
    internet.Install(server);

    // -----------------------------------------------------
   // -----------------------------------------------------
    // Direct wired SFL topology
    //
    // Central server has one independent point-to-point
    // connection to each silo.
    //
    //          Server
    //        / / / | \ \ \
    //      S0 S1 S2 ... S(numSilos-1)
    // -----------------------------------------------------

    std::vector<Ipv4Address> serverAddress(numSilos);
    std::vector<Ipv4Address> siloAddress(numSilos);

    // Optional: use same bandwidth/delay for every silo.
    // We can make these heterogeneous later.
    const std::string linkRate = "1Mbps";
    const double linkDelayMs = 10.0;

    for (uint32_t i = 0; i < numSilos; ++i)
    {
        PointToPointHelper p2p;

        p2p.SetDeviceAttribute(
            "DataRate",
            StringValue(linkRate)
        );

        p2p.SetChannelAttribute(
            "Delay",
            TimeValue(
                MilliSeconds(linkDelayMs)
            )
        );

        // Server <-> Silo i
        NodeContainer pair;

        pair.Add(server);
        pair.Add(silos.Get(i));

        NetDeviceContainer devices =
            p2p.Install(pair);

        // Each server-silo link gets its own subnet:
        //
        // silo 0: 10.1.1.0/24
        // silo 1: 10.1.2.0/24
        // ...
        // silo 10: 10.1.11.0/24

        std::ostringstream network;

        network
            << "10.1."
            << (i + 1)
            << ".0";

        Ipv4AddressHelper ipv4;

        ipv4.SetBase(
            network.str().c_str(),
            "255.255.255.0"
        );

        Ipv4InterfaceContainer interfaces =
            ipv4.Assign(devices);

        // pair.Add(server) was first,
        // therefore interface 0 belongs to server.
        serverAddress[i] =
            interfaces.GetAddress(0);

        // pair.Add(silos.Get(i)) was second,
        // therefore interface 1 belongs to silo i.
        siloAddress[i] =
            interfaces.GetAddress(1);
    }

    Ipv4GlobalRoutingHelper::
        PopulateRoutingTables();

    

    // -----------------------------------------------------
    // Determine participating silos
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
        selectedSilos =
            ParseSiloList(activeSilos);
    }

    const double startTime = 1.0;

    uint16_t basePort = 9000;
    uint16_t maxPort =
        basePort +
        static_cast<uint16_t>(numSilos - 1);

    // -----------------------------------------------------
    // Schedule TCP transfers
    // -----------------------------------------------------

    for (
        uint32_t index = 0;
        index < selectedSilos.size();
        ++index
    )
    {
        uint32_t silo =
            selectedSilos[index];

        uint32_t senderId;
        uint32_t receiverId;

        Ptr<Node> senderNode;
        Ptr<Node> receiverNode;

        Ipv4Address receiverAddress;
    

    if (phase == "download")
    {
        // Server -> Silo
        senderId = SERVER_ID;
        receiverId = silo;

        senderNode = server;
        receiverNode = silos.Get(silo);

        // Destination is the silo-side IP
        // of this silo's direct link.
        receiverAddress =
            siloAddress[silo];
   
    }
    else
    {
        // Silo -> Server
        senderId = silo;
        receiverId = SERVER_ID;

        senderNode = silos.Get(silo);
        receiverNode = server;

        // Destination is the server-side IP
        // of this silo's direct link.
        receiverAddress =
            serverAddress[silo];
    }

        // Uploads all terminate at the same server,
        // therefore every transfer must use a unique port.
        uint16_t port =
            basePort +
            static_cast<uint16_t>(silo);

        // -------------------------------------------------
        // TCP receiver
        // -------------------------------------------------

        PacketSinkHelper sinkHelper(
            "ns3::TcpSocketFactory",
            InetSocketAddress(
                Ipv4Address::GetAny(),
                port
            )
        );

        ApplicationContainer sinkApps =
            sinkHelper.Install(receiverNode);

        sinkApps.Start(
            Seconds(0.5)
        );

        sinkApps.Stop(
            Seconds(
                startTime +
                deadline +
                0.5
            )
        );

        Ptr<PacketSink> sink =
            DynamicCast<PacketSink>(
                sinkApps.Get(0)
            );

        // -------------------------------------------------
        // TCP sender
        // -------------------------------------------------

        BulkSendHelper sender(
            "ns3::TcpSocketFactory",
            InetSocketAddress(
                receiverAddress,
                port
            )
        );

        sender.SetAttribute(
            "MaxBytes",
            UintegerValue(modelBytes)
        );

        ApplicationContainer senderApps =
            sender.Install(senderNode);

        senderApps.Start(
            Seconds(startTime)
        );

        senderApps.Stop(
            Seconds(
                startTime +
                deadline
            )
        );

        TransferInfo info;

        info.silo = silo;
        info.sender = senderId;
        info.receiver = receiverId;

        info.expectedBytes =
            modelBytes;

        info.startTime =
            startTime;

        info.deadlineTime =
            startTime + deadline;

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

    // -----------------------------------------------------
    // Observe completion
    // -----------------------------------------------------

    Simulator::Schedule(
        Seconds(startTime),
        &CheckTransfers
    );

    Simulator::Stop(
        Seconds(
            startTime +
            deadline +
            0.6
        )
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

        fileExists =
            testFile.good();
    }

    std::ofstream log(
        logFile,
        std::ios::app
    );

    if (!log.is_open())
    {
        NS_FATAL_ERROR(
            "Cannot open network log file: "
            << logFile
        );
    }

    if (!fileExists)
    {
        log
            << "round,"
            << "phase,"
            << "flow_id,"
            << "silo,"
            << "flow_type,"
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
        FlowId flowId =
            entry.first;

        const FlowMonitor::FlowStats& st =
            entry.second;

        Ipv4FlowClassifier::FiveTuple tuple =
            classifier->FindFlow(flowId);

        std::string flowType;

        if (
            tuple.destinationPort >= basePort &&
            tuple.destinationPort <= maxPort
        )
        {
            flowType = "MODEL_DATA";
        }
        else if (
            tuple.sourcePort >= basePort &&
            tuple.sourcePort <= maxPort
        )
        {
            flowType = "TCP_ACK";
        }
        else
        {
            flowType = "OTHER";
        }

        int32_t siloId = -1;

        if (
            tuple.destinationPort >= basePort &&
            tuple.destinationPort <= maxPort
        )
        {
            siloId =
                static_cast<int32_t>(
                    tuple.destinationPort - basePort
                );
        }
        else if (
            tuple.sourcePort >= basePort &&
            tuple.sourcePort <= maxPort
        )
        {
            siloId =
                static_cast<int32_t>(
                    tuple.sourcePort - basePort
                );
        }
        bool modelSuccess = false;

        if (siloId >= 0)
        {
            for (const auto& t : g_transfers)
            {
                if (
                    t.silo ==
                    static_cast<uint32_t>(siloId)
                )
                {
                    uint64_t receivedBytes =
                        t.sink->GetTotalRx();

                    modelSuccess =
                        t.finished &&
                        receivedBytes >= t.expectedBytes &&
                        t.finishTime <= t.deadlineTime;

                    break;
                }
            }
        }

        double packetLossPercent = 0.0;

        if (st.txPackets > 0)
        {
            packetLossPercent =
                100.0 *
                static_cast<double>(
                    st.lostPackets
                ) /
                static_cast<double>(
                    st.txPackets
                );
        }

        double meanDelayMs = 0.0;

        if (st.rxPackets > 0)
        {
            meanDelayMs =
                st.delaySum.GetSeconds()
                /
                static_cast<double>(
                    st.rxPackets
                )
                * 1000.0;
        }

        double throughputMbps = 0.0;

        if (
            st.rxPackets > 0 &&
            st.timeLastRxPacket >
            st.timeFirstTxPacket
        )
        {
            double duration =
                (
                    st.timeLastRxPacket -
                    st.timeFirstTxPacket
                ).GetSeconds();

            throughputMbps =
                (
                    static_cast<double>(
                        st.rxBytes
                    ) *
                    8.0
                )
                /
                duration
                /
                1000000.0;
        }

        log
            << round << ","
            << phase << ","
            << flowId << ","
            << siloId << ","
            << flowType << ","
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
            << std::fixed
            << std::setprecision(4)
            << packetLossPercent << ","
            << meanDelayMs << ","
            << throughputMbps
            << std::endl;
    }

    log.close();

    // -----------------------------------------------------
    // Machine-readable output for Python
    // -----------------------------------------------------

    for (const auto& t : g_transfers)
    {
        uint64_t bytes =
            t.sink->GetTotalRx();

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
            << " success="
            << (success ? 1 : 0)
            << " time="
            << transferTime
            << " bytes="
            << bytes
            << std::endl;
    }

    Simulator::Destroy();

    return 0;
}