#include "ns3/core-module.h"
#include "ns3/network-module.h"
#include "ns3/point-to-point-module.h"
#include "ns3/wifi-module.h"
#include "ns3/mobility-module.h"
#include "ns3/netanim-module.h"

#include "ns3/ndnSIM-module.h"

#include <cmath>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

using namespace ns3;

NS_LOG_COMPONENT_DEFINE("GaiaSflWifiNdn");

int
main(int argc, char* argv[])
{
    // -----------------------------------------------------
    // Parameters
    // -----------------------------------------------------

    std::string phase = "download";

    uint32_t round = 1;

    uint32_t numSilos = 11;

    uint64_t modelBytes =
        341980ULL * 4ULL;

    double deadline = 25.0;

    uint32_t payloadSize = 4096;

    uint32_t windowSize = 20;

    std::string consumerType = "window";

    double cbrFrequency = 20.0;

    std::string runId = "";

    std::string activeSilos = "";

    uint32_t uploadBatchSize = 0;

    double startJitter = 0.5;

    bool enableAnim = false;

    CommandLine cmd;

    cmd.AddValue(
        "phase",
        "download only for first NDN test",
        phase
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
        "modelBytes",
        "FADNet model size in bytes",
        modelBytes
    );

    cmd.AddValue(
        "deadline",
        "Communication deadline in seconds",
        deadline
    );

    cmd.AddValue(
        "payloadSize",
        "NDN Data payload size",
        payloadSize
    );

    cmd.AddValue(
        "window",
        "ConsumerWindow initial window (only used when "
        "consumerType=window)",
        windowSize
    );

    cmd.AddValue(
        "consumerType",
        "\"window\" (default) uses ns3::ndn::ConsumerWindow, "
        "an AIMD-style flow control that can burst aggressively "
        "and hard-resets on any timeout. \"cbr\" uses "
        "ns3::ndn::ConsumerCbr instead, which sends Interests at "
        "a fixed, open-loop rate (see cbrFrequency) with no "
        "burst/collapse dynamics — useful for keeping many "
        "concurrent uploaders' aggregate offered load under "
        "control on a shared WiFi channel.",
        consumerType
    );

    cmd.AddValue(
        "cbrFrequency",
        "Interests per second per silo when consumerType=cbr",
        cbrFrequency
    );

    cmd.AddValue(
        "uploadBatchSize",
        "For phase=upload only: how many silos are allowed to "
        "transmit on the shared WiFi channel at once. 0 (default) "
        "means every silo starts together, same as before. A "
        "positive value splits silos into sequential groups of "
        "that size, each group getting its own full deadline-length "
        "window (staggered in simulated time), to cut down uplink "
        "collisions from too many concurrent transmitters.",
        uploadBatchSize
    );

    cmd.AddValue(
        "startJitter",
        "Max random offset (seconds) added to each silo's own "
        "start time, drawn uniformly from [0, startJitter). "
        "Desynchronizes the initial burst so all silos don't fire "
        "their first Interest/Data in the exact same instant, which "
        "otherwise maximizes the very first collision. 0 disables it.",
        startJitter
    );

    cmd.AddValue(
        "enableAnim",
        "Write a NetAnim XML trace for this run "
        "(off by default: it adds overhead, so only "
        "turn it on for the run you want to inspect)",
        enableAnim
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
    // numSilos silos + one central server
    // -----------------------------------------------------

    NodeContainer silos;
    silos.Create(numSilos);

    Ptr<Node> server =
        CreateObject<Node>();

    Ptr<Node> rsu =
        CreateObject<Node>();

    NodeContainer allNodes;

    allNodes.Add(silos);
    allNodes.Add(server);
    allNodes.Add(rsu);

    // -----------------------------------------------------
    // Server <-> RSU <-> every silo. The RSU is a wired AP;
    // every silo is a WiFi station associated to it, all
    // sharing the same channel (e.g. vehicles pulling the
    // global model from a road-side unit over WiFi).
    //
    //           Server
    //             |  (wired backhaul)
    //            RSU  == (AP)
    //          )) )) )) ))   (WiFi cell, shared medium)
    //        S0 S1 S2 ... S(numSilos-1)
    //
    // WiFi cell     (RSU    <-> silos): 802.11a, fixed PHY rate
    // Backhaul link (Server <-> RSU):   100 Mbps, 2 ms
    // -----------------------------------------------------

    const std::string backhaulRate =
        "100Mbps";

    const double backhaulDelayMs =
        2.0;

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

        p2p.Install(pair);
    }

    // Real-world WiFi realism knobs (all standard ns-3 WiFi config,
    // nothing ndnSIM/simulator-core specific):
    //  - RTS/CTS enabled for anything above 500 bytes, which is what
    //    real routers use to fight the multi-station hidden-terminal
    //    collision problem we ran into with 11 concurrent uploaders.
    //  - 802.11n (5GHz) instead of the essentially-obsolete 802.11a,
    //    since that's what an actual consumer/road-side router runs.
    //  - Minstrel rate control instead of a fixed PHY rate, since real
    //    routers dynamically probe and pick the best rate per client
    //    rather than using one fixed rate for everyone. This does mean
    //    results are no longer bit-identical run to run (real WiFi rate
    //    adaptation is itself somewhat stochastic).
    //  - 20 dBm TX power, a typical real consumer router's max legal
    //    transmit power.
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
    wifi.SetRemoteStationManager(
        "ns3::MinstrelHtWifiManager"
    );

    Ssid ssid = Ssid("gaia-sfl-rsu");
    WifiMacHelper wifiMac;

    // Silos are stations associated to the RSU's access point
    wifiMac.SetType(
        "ns3::StaWifiMac",
        "Ssid", SsidValue(ssid),
        "ActiveProbing", BooleanValue(false)
    );

    NetDeviceContainer staDevices =
        wifi.Install(wifiPhy, wifiMac, silos);

    // RSU is the access point all silos share the channel with
    wifiMac.SetType(
        "ns3::ApWifiMac",
        "Ssid", SsidValue(ssid)
    );

    NodeContainer apNode;
    apNode.Add(rsu);

    NetDeviceContainer apDevice =
        wifi.Install(wifiPhy, wifiMac, apNode);

    // -----------------------------------------------------
    // Node positions (RSU at the center, server pulled out
    // above it, silos in a ring around the RSU). These now
    // also feed the WiFi propagation loss model (Friis uses
    // actual node distance), not just NetAnim.
    // -----------------------------------------------------

    MobilityHelper mobility;

    Ptr<ListPositionAllocator> positionAlloc =
        CreateObject<ListPositionAllocator>();

    const double kPi = 3.14159265358979323846;
    const double radius = 50.0;

    // RSU at the center
    positionAlloc->Add(
        Vector(0.0, 0.0, 0.0)
    );

    // Server above the ring, connected only to the RSU
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
// Install NDN stack
// -----------------------------------------------------

ns3::ndn::StackHelper ndnHelper;
ndnHelper.InstallAll();

// -----------------------------------------------------
// Forwarding strategy
// -----------------------------------------------------

ns3::ndn::StrategyChoiceHelper::InstallAll(
    "/sfl",
    "/localhost/nfd/strategy/best-route"
);

// -----------------------------------------------------
// Global NDN routing helper
// -----------------------------------------------------

ns3::ndn::GlobalRoutingHelper routingHelper;
routingHelper.InstallAll();

// -----------------------------------------------------
// Timing
// -----------------------------------------------------

const double startTime = 1.0;
const double stopTime =
    startTime + deadline;

// -----------------------------------------------------
// Model size
// -----------------------------------------------------

double modelSizeMiB =
    static_cast<double>(modelBytes)
    /
    (1024.0 * 1024.0);

// -----------------------------------------------------
// Producer/consumer pairs
// -----------------------------------------------------

Ptr<UniformRandomVariable> jitterRand =
    CreateObject<UniformRandomVariable>();

for (uint32_t silo = 0;
     silo < numSilos;
     ++silo)
{
    std::ostringstream prefix;

    if (phase == "download")
    {
        // Shared name for every silo (no /silo/<id> suffix) so the
        // RSU's PIT aggregates concurrent requests and its CS can
        // serve the cached global model to stragglers.
        prefix
            << "/sfl/global/round/"
            << round
            << "/silo/";
    }
    else
    {
        prefix
            << "/sfl/local/round/"
            << round
            << "/silo/"
            << silo;
    }

    Ptr<Node> producerNode;
    Ptr<Node> consumerNode;

    if (phase == "download")
    {
        // Server owns global model
        producerNode = server;

        // Silo requests global model
        consumerNode = silos.Get(silo);
    }
    else
    {
        // Silo owns its locally trained model
        producerNode = silos.Get(silo);

        // Server requests local model
        consumerNode = server;
    }

    // -----------------------------------------------------
    // Per-silo timing. Downloads (and uploads when
    // uploadBatchSize == 0) all share one deadline-length
    // window starting at startTime, same as before. When
    // uploadBatchSize > 0, silos are split into sequential
    // groups of that size, each group getting its own full
    // deadline-length window later in simulated time, so at
    // most uploadBatchSize silos ever transmit on the shared
    // WiFi channel at once.
    // -----------------------------------------------------

    double siloStartTime = startTime;
    double siloStopTime = stopTime;

    if (phase == "upload" && uploadBatchSize > 0)
    {
        uint32_t batchIndex = silo / uploadBatchSize;

        siloStartTime =
            startTime +
            static_cast<double>(batchIndex) * deadline;

        siloStopTime = siloStartTime + deadline;
    }

    // Desynchronize the initial burst: without this, every silo's
    // first Interest/Data fires at the exact same simulated instant,
    // which maximizes the very first collision. A small random
    // per-silo offset spreads that out. CSMA/CA's own backoff is
    // still randomized on top of this — this just avoids starting
    // every flow perfectly aligned in the first place.
    if (startJitter > 0.0)
    {
        double jitter = jitterRand->GetValue(0.0, startJitter);

        siloStartTime += jitter;
        siloStopTime += jitter;
    }

    // ---------------------------------------------
    // Producer
    // ---------------------------------------------

    ns3::ndn::AppHelper producer(
        "ns3::ndn::Producer"
    );

    producer.SetPrefix(
        prefix.str()
    );

    producer.SetAttribute(
        "PayloadSize",
        UintegerValue(payloadSize)
    );

    producer.Install(
        producerNode
    );

    routingHelper.AddOrigins(
        prefix.str(),
        producerNode
    );

    // ---------------------------------------------
    // Consumer
    // ---------------------------------------------

    ns3::ndn::AppHelper consumer(
        consumerType == "cbr"
            ? "ns3::ndn::ConsumerCbr"
            : "ns3::ndn::ConsumerWindow"
    );

    consumer.SetPrefix(
        prefix.str()
    );

    if (consumerType == "cbr")
    {
        // ConsumerCbr has no PayloadSize/Size attributes of its
        // own (those only shape the producer's response and the
        // ConsumerWindow's Size->MaxSeq conversion) — compute
        // MaxSeq ourselves the same way Ns3NdnBackend does on
        // the Python side, so both sides agree on chunk count.
        uint32_t totalChunks =
            (modelBytes + payloadSize - 1) / payloadSize;

        consumer.SetAttribute(
            "Frequency",
            DoubleValue(cbrFrequency)
        );

        consumer.SetAttribute(
            "MaxSeq",
            IntegerValue(totalChunks - 1)
        );
    }
    else
    {
        consumer.SetAttribute(
            "Window",
            UintegerValue(windowSize)
        );

        consumer.SetAttribute(
            "PayloadSize",
            UintegerValue(payloadSize)
        );

        consumer.SetAttribute(
            "Size",
            DoubleValue(modelSizeMiB)
        );
    }

    ApplicationContainer consumerApp =
        consumer.Install(
            consumerNode
        );

    consumerApp.Start(
        Seconds(siloStartTime)
    );

    consumerApp.Stop(
        Seconds(siloStopTime)
    );
}

// -----------------------------------------------------
// Calculate routes
// -----------------------------------------------------

ns3::ndn::GlobalRoutingHelper::
    CalculateRoutes();

// -----------------------------------------------------
// Tracers
// -----------------------------------------------------
std::string l3Trace =
    "/home/hurair/ndnSIM/ns-3/"
    "scratch/gaia-sfl-ndn-wifi/"
    "wifi_ndn_" +
    phase +
    "_l3_rate.txt";

std::string delayTrace =
    "/home/hurair/ndnSIM/ns-3/"
    "scratch/gaia-sfl-ndn-wifi/"
    "wifi_ndn_" +
    phase +
    "_app_delay.txt";
ns3::ndn::L3RateTracer::InstallAll(
    l3Trace,
    Seconds(1.0)
);

ns3::ndn::AppDelayTracer::InstallAll(
    delayTrace
);

// -----------------------------------------------------
// NetAnim (opt-in via --enableAnim=1)
// -----------------------------------------------------

std::unique_ptr<AnimationInterface> anim;

if (enableAnim)
{
    std::string animFile =
        "/home/hurair/ndnSIM/ns-3/"
        "scratch/gaia-sfl-ndn-wifi/"
        "wifi_ndn_" +
        phase +
        "_anim.xml";

    anim = std::make_unique<AnimationInterface>(
        animFile
    );

    anim->EnablePacketMetadata(true);

    anim->UpdateNodeDescription(server, "Server");
    anim->UpdateNodeColor(server, 255, 0, 0);

    anim->UpdateNodeDescription(rsu, "RSU");
    anim->UpdateNodeColor(rsu, 0, 255, 0);

    for (uint32_t i = 0; i < numSilos; ++i)
    {
        anim->UpdateNodeDescription(
            silos.Get(i),
            "Silo " + std::to_string(i)
        );

        anim->UpdateNodeColor(
            silos.Get(i),
            0, 0, 255
        );
    }

    std::cout
        << "[NetAnim] writing " << animFile
        << std::endl;
}

// -----------------------------------------------------
// Simulation
// -----------------------------------------------------

double totalStopTime = stopTime;

if (phase == "upload" && uploadBatchSize > 0)
{
    uint32_t numBatches =
        (numSilos + uploadBatchSize - 1) / uploadBatchSize;

    totalStopTime =
        startTime +
        static_cast<double>(numBatches) * deadline;
}

// +startJitter so the last, most-delayed silo's own (jittered)
// stop time never gets clipped by the global simulation stop.
Simulator::Stop(
    Seconds(totalStopTime + startJitter + 0.5)
);

Simulator::Run();

Simulator::Destroy();

return 0;
}