#include "ns3/core-module.h"
#include "ns3/network-module.h"
#include "ns3/point-to-point-module.h"
#include "ns3/wifi-module.h"
#include "ns3/mobility-module.h"
#include "ns3/netanim-module.h"

#include "ns3/ndnSIM-module.h"

#include <algorithm>
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

    double cbrFrequency = 100.0;

    std::string cbrRandomize = "none";

    std::string runId = "";

    std::string activeSilos = "";

    uint32_t uploadBatchSize = 0;

    double startJitter = 0.5;

    std::string wifiMode = "infra";

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
        "cbrFrequency",
        "Interests per second per silo (ns3::ndn::ConsumerCbr's "
        "fixed, open-loop rate). Used for both download and upload — "
        "ConsumerWindow's AIMD (uncapped growth on success, hard "
        "reset on any timeout) caused uneven/incomplete downloads "
        "even with a single broadcaster, and a full starvation "
        "collapse under upload's multi-concurrent-broadcaster "
        "contention. ConsumerCbr's steady pacing fixed both.",
        cbrFrequency
    );

    cmd.AddValue(
        "cbrRandomize",
        "ns3::ndn::ConsumerCbr's own \"Randomize\" attribute: "
        "\"none\" (default, fixed 1/Frequency spacing), \"uniform\", "
        "or \"exponential\" — jitters every Interest's send time "
        "throughout the run, not just the flow's first one (that's "
        "startJitter, a separate, one-time offset).",
        cbrRandomize
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
        "activeSilos",
        "Comma-separated list of the specific silo IDs that should "
        "actually generate traffic this run (e.g. \"5,6,7,9\"). Empty "
        "(default) means every silo 0..numSilos-1 is active. The WiFi "
        "topology still creates all numSilos stations either way (node "
        "count/positions are unaffected) — this only controls which "
        "ones get a Producer/Consumer installed, so the caller isn't "
        "forced to simulate contention from silos it never asked for "
        "(e.g. numSilos=10 to cover silo IDs up to 9 would otherwise "
        "spin up full traffic for silos 0-4 and 8 too, even if only "
        "5, 6, 7, 9 were actually requested).",
        activeSilos
    );

    cmd.AddValue(
        "wifiMode",
        "\"infra\" (default): RSU is a real AP (ns3::ApWifiMac), "
        "silos are stations (ns3::StaWifiMac) that associate to it — "
        "matches a real roadside WiFi router. \"adhoc\": every node "
        "(RSU and silos) uses ns3::AdhocWifiMac instead — no AP role, "
        "no association handshake. Both modes use the identical "
        "underlying CSMA/CA (ns3::RegularWifiMac base), so this does "
        "not change collision-avoidance behavior — it only removes "
        "the association step (already eliminated separately by "
        "sizing numSilos to just the active silos) and reflects the "
        "OCB-style, connectionless pattern real 802.11p V2X uses.",
        wifiMode
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

    WifiMacHelper wifiMac;

    NetDeviceContainer staDevices;
    NetDeviceContainer apDevice;

    if (wifiMode == "adhoc")
    {
        // No AP, no association — every node (RSU included) is a
        // symmetric peer on the same channel, OCB-style.
        wifiMac.SetType(
            "ns3::AdhocWifiMac"
        );

        staDevices = wifi.Install(wifiPhy, wifiMac, silos);
        apDevice = wifi.Install(wifiPhy, wifiMac, rsu);
    }
    else
    {
        Ssid ssid = Ssid("gaia-sfl-rsu");

        // Silos are stations associated to the RSU's access point
        wifiMac.SetType(
            "ns3::StaWifiMac",
            "Ssid", SsidValue(ssid),
            "ActiveProbing", BooleanValue(false)
        );

        staDevices = wifi.Install(wifiPhy, wifiMac, silos);

        // RSU is the access point all silos share the channel with
        wifiMac.SetType(
            "ns3::ApWifiMac",
            "Ssid", SsidValue(ssid)
        );

        NodeContainer apNode;
        apNode.Add(rsu);

        apDevice = wifi.Install(wifiPhy, wifiMac, apNode);
    }

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
// Producer/consumer pairs
// -----------------------------------------------------

Ptr<UniformRandomVariable> jitterRand =
    CreateObject<UniformRandomVariable>();

// Download: every silo asks for the exact same global-model name
// (no /silo/<id> suffix) so the RSU's PIT aggregates concurrent
// requests and its CS can serve the cached global model to
// stragglers. Since the name never varies by silo, the producer
// only needs to be installed once, here, before the silo loop —
// installing it again per silo would just be numSilos redundant
// Producer apps on the server for the exact same name.
std::ostringstream downloadPrefix;
downloadPrefix
    << "/sfl/global/round/"
    << round
    << "/silo/";

if (phase == "download")
{
    ns3::ndn::AppHelper globalProducer(
        "ns3::ndn::Producer"
    );

    globalProducer.SetPrefix(
        downloadPrefix.str()
    );

    globalProducer.SetAttribute(
        "PayloadSize",
        UintegerValue(payloadSize)
    );

    globalProducer.Install(
        server
    );

    routingHelper.AddOrigins(
        downloadPrefix.str(),
        server
    );
}

// Which silo IDs actually get a Producer/Consumer installed this
// run. Empty activeSilos (the default) means all of them, preserving
// prior behavior. The list is sorted because for upload, all
// consumers live on the same node (the server), so ns-3 assigns
// their AppIds by installation order (0, 1, 2, ...) — the Nth
// installed app is AppId N. Ns3NdnBackend.py maps AppId back to a
// real silo ID via the same sorted order, so this order must match.
std::vector<uint32_t> activeSiloList;

if (activeSilos.empty())
{
    for (uint32_t i = 0; i < numSilos; ++i)
    {
        activeSiloList.push_back(i);
    }
}
else
{
    std::stringstream activeSilosStream(activeSilos);
    std::string token;

    while (std::getline(activeSilosStream, token, ','))
    {
        activeSiloList.push_back(
            static_cast<uint32_t>(std::stoul(token))
        );
    }

    std::sort(
        activeSiloList.begin(),
        activeSiloList.end()
    );
}

for (uint32_t loopIdx = 0;
     loopIdx < activeSiloList.size();
     ++loopIdx)
{
    uint32_t silo = activeSiloList[loopIdx];

    std::ostringstream prefix;

    if (phase == "download")
    {
        prefix << downloadPrefix.str();
    }
    else
    {
        prefix
            << "/sfl/local/round/"
            << round
            << "/silo/"
            << silo;
    }

    // Upload only: each silo owns a distinct local model, so
    // (unlike download) it needs its own Producer, installed
    // below inside this loop.
    Ptr<Node> producerNode;
    Ptr<Node> consumerNode;

    // silos.Get() is indexed by loopIdx (position among the silos
    // actually active this run), not by the real silo ID — the WiFi
    // topology only ever has activeSiloList.size() stations (see
    // numSilos, sized by Ns3NdnBackend to match), so a sparse real ID
    // like 9 would be out of bounds if used as a station index here.
    // The real silo ID is still what goes into the NDN name (via
    // `silo` elsewhere), so results still map back to the right silo.
    if (phase == "download")
    {
        // Silo requests global model
        consumerNode = silos.Get(loopIdx);
    }
    else
    {
        // Silo owns its locally trained model
        producerNode = silos.Get(loopIdx);

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
        // Grouped by position among the active silos (loopIdx), not
        // by real silo ID — real IDs can be sparse (e.g. 5,6,7,9),
        // which wouldn't divide into clean, evenly-sized batches.
        uint32_t batchIndex = loopIdx / uploadBatchSize;

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
    // Producer (upload only — download's producer was already
    // installed once, above the loop)
    // ---------------------------------------------

    if (phase == "upload")
    {
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
    }

    // ---------------------------------------------
    // Consumer
    // ---------------------------------------------

    ns3::ndn::AppHelper consumer(
        "ns3::ndn::ConsumerCbr"
    );

    consumer.SetPrefix(
        prefix.str()
    );

    // ConsumerCbr has no PayloadSize/Size attribute of its own (those
    // only shape the producer's response), so MaxSeq is computed
    // manually here. It's an EXCLUSIVE bound in Consumer::SendPacket()
    // ("if (m_seq >= m_seqMax) return"), so it must equal totalChunks
    // (not totalChunks - 1) to actually send sequence numbers
    // 0..totalChunks-1 — confirmed against ConsumerWindow's own
    // Size->MaxSeq formula, which computes exactly
    // floor(1 + sizeBytes/payloadSize) == totalChunks.
    uint32_t totalChunks =
        (modelBytes + payloadSize - 1) / payloadSize;

    consumer.SetAttribute(
        "Frequency",
        DoubleValue(cbrFrequency)
    );

    consumer.SetAttribute(
        "Randomize",
        StringValue(cbrRandomize)
    );

    consumer.SetAttribute(
        "MaxSeq",
        IntegerValue(totalChunks)
    );

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