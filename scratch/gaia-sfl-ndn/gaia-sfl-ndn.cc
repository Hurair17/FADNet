#include "ns3/core-module.h"
#include "ns3/network-module.h"
#include "ns3/point-to-point-module.h"
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

NS_LOG_COMPONENT_DEFINE("GaiaSflWiredNdn");

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

    double deadline = 20.0;

    uint32_t payloadSize = 4096;

    uint32_t windowSize = 20;

    std::string runId = "";

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
        "ConsumerWindow initial window",
        windowSize
    );

    cmd.AddValue(
        "runId",
        "Unique tag inserted into trace filenames so concurrent "
        "ns-3 invocations (e.g. different rounds/phases running "
        "at once) never overwrite each other's trace files",
        runId
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
    // Server <-> RSU <-> every silo. The RSU is a single
    // relay all silo traffic passes through (e.g. a shared
    // road-side unit between the FL server and the silos).
    //
    //           Server
    //             |
    //            RSU
    //          / | | \
    //        S0 S1 S2 ... S(numSilos-1)
    //
    // Access link  (RSU    <-> silo):  1 Mbps, 10 ms
    // Backhaul link (Server <-> RSU): 10 Mbps,  2 ms
    // -----------------------------------------------------

    const std::string accessRate =
        "1Mbps";

    const double accessDelayMs =
        10.0;

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

    for (uint32_t i = 0; i < numSilos; ++i)
    {
        PointToPointHelper p2p;

        p2p.SetDeviceAttribute(
            "DataRate",
            StringValue(accessRate)
        );

        p2p.SetChannelAttribute(
            "Delay",
            TimeValue(
                MilliSeconds(accessDelayMs)
            )
        );

        NodeContainer pair;

        pair.Add(rsu);
        pair.Add(silos.Get(i));

        p2p.Install(pair);
    }

    // -----------------------------------------------------
    // Node positions (RSU at the center, server pulled out
    // above it, silos in a ring around the RSU) — only
    // matters for NetAnim, but cheap to always set.
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
        "ns3::ndn::ConsumerWindow"
    );

    consumer.SetPrefix(
        prefix.str()
    );

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

    ApplicationContainer consumerApp =
        consumer.Install(
            consumerNode
        );

    consumerApp.Start(
        Seconds(startTime)
    );

    consumerApp.Stop(
        Seconds(stopTime)
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

// Empty runId keeps the old plain filenames (convenient for
// manual ./waf --run debugging); a non-empty runId (always
// set by the Python training loop) makes every invocation's
// trace files unique so concurrent runs can't clobber each
// other's output.
std::string runSuffix =
    runId.empty() ? "" : ("_" + runId);

std::string l3Trace =
    "/home/hurair/ndnSIM/ns-3/"
    "scratch/gaia-sfl-ndn/"
    "wired_ndn_" +
    phase +
    runSuffix +
    "_l3_rate.txt";

std::string delayTrace =
    "/home/hurair/ndnSIM/ns-3/"
    "scratch/gaia-sfl-ndn/"
    "wired_ndn_" +
    phase +
    runSuffix +
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
        "scratch/gaia-sfl-ndn/"
        "wired_ndn_" +
        phase +
        runSuffix +
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

Simulator::Stop(
    Seconds(stopTime + 0.5)
);

Simulator::Run();

Simulator::Destroy();

return 0;
}