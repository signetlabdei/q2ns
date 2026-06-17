/*-----------------------------------------------------------------------------
 * Q2NS - Quantum Network Simulator
 * Copyright (c) 2026 quantuminternet.it
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *---------------------------------------------------------------------------*/
/**
 * @file q2nsviz-channel-loss-example.cc
 * @brief Entanglement distribution with photon loss, receiver-side NACK, and retry.
 *
 * Source generates a Bell pair and sends the flying qubit over a lossy channel.
 * A classical notification packet is sent concurrently so Destination can arm a
 * receive-timeout.  On qubit arrival Destination sends an ACK, on timeout it sends
 * a NACK.
 *
 * Tunable parameters:
 *   --run        RNG run index (default 1).
 *   --loss-prob  Per-qubit loss probability [0,1] (default 0.8).
 *
 * Timing model (illustrative):
 *   kSingleGate = 100 ns  (single-qubit gate)
 *   kTwoQGate   = 300 ns  (two-qubit gate)
 *   kQDelay     =  10 ns  (quantum channel propagation, ~2 m fiber)
 *   kDstTimeout =   5 ns  (scheduled from notification arrival; qubit arrives ~2.5 ns
 *                          before the notify packet at 100 Mbps, so the timeout fires
 *                          safely after a successful reception and before any retry)
 *
 * Visualization output is written to
 * examples/example_traces/q2nsviz-channel-loss-example.json and can be loaded in the
 * q2nsviz viewer (src/q2ns/utils/q2nsviz-serve.sh).
 */


#include "ns3/core-module.h"
#include "ns3/internet-module.h"
#include "ns3/network-module.h"
#include "ns3/point-to-point-module.h"
#include "ns3/simulator.h"

#include "ns3/q2ns-netcontroller.h"
#include "ns3/q2ns-qgate.h"
#include "ns3/q2ns-qmap.h"
#include "ns3/q2ns-qnode.h"
#include "ns3/q2ns-qubit.h"

#include "ns3/q2nsviz-trace-writer.h"
#include "ns3/q2nsviz-trace.h"

#include <functional>
#include <iostream>

using namespace ns3;
using namespace q2ns;

static const uint16_t kCtrlPort = 9200;     // Destination -> Source (ACK/NACK)
static const uint16_t kNotifyPort = 9201;   // Source -> Destination (qubit notification)
static const int kMaxAttempts = 20;

int main(int argc, char** argv) {
  std::cout << "[DEMO] Channel-loss entanglement distribution starting\n";

  uint32_t run = 1;
  double lossProb = 0.8;

  RngSeedManager::SetSeed(1);

  CommandLine cmd;
  cmd.AddValue("run", "RNG run index", run);
  cmd.AddValue("loss-prob", "Per-qubit loss probability [0,1]", lossProb);
  cmd.Parse(argc, argv);

  RngSeedManager::SetRun(run);

  TraceWriter::Instance().Open(Q2NS_EXAMPLE_TRACES_DIR "q2nsviz-channel-loss-example.json");
  Trace("Entanglement Distribution with Photon Loss (loss-prob=", lossProb, " run=", run, ")");

  Time::SetResolution(Time::NS);

  NetController net;
  net.SetQStateBackend(QStateBackend::Ket);

  // --- Nodes ---
  auto src = net.CreateNode();
  auto dst = net.CreateNode();

  TraceCreateNode("Source", 25, 50);
  TraceCreateNode("Destination", 75, 50);

  // --- Timing constants ---
  const Time kSingleGate = NanoSeconds(100);
  const Time kTwoQGate = NanoSeconds(300);
  const Time kQDelay = NanoSeconds(10);
  const Time kClassical = kQDelay;
  // kDstTimeout is scheduled from notification arrival (not from qubit send time).
  // Timeout fires at t_notify + 5 ns (before a new attempt is made)
  const Time kDstTimeout = NanoSeconds(5);

  // --- Quantum link (lossy) ---
  auto ch = net.InstallQuantumLink(src, dst);
  ch->SetAttribute("Delay", TimeValue(kQDelay));

  auto lossMap = CreateObject<LossQMap>();
  lossMap->SetAttribute("Probability", DoubleValue(lossProb));
  ch->SetQMap(lossMap);

  TraceCreateChannel("Source", "Destination", "quantum");

  // --- Classical network ---
  InternetStackHelper internet;
  internet.Install(src);
  internet.Install(dst);

  PointToPointHelper p2p;
  p2p.SetDeviceAttribute("DataRate", StringValue("100Mbps"));
  p2p.SetChannelAttribute("Delay", StringValue("10ns"));

  Ipv4AddressHelper ipv4;
  ipv4.SetBase("10.1.1.0", "255.255.255.0");
  auto ifsSD = ipv4.Assign(p2p.Install(src, dst));
  Ipv4GlobalRoutingHelper::PopulateRoutingTables();
  net.AssignStreams(0);

  const Ipv4Address kSrcAddr = ifsSD.GetAddress(0);
  const Ipv4Address kDstAddr = ifsSD.GetAddress(1);

  TraceCreateChannel("Destination", "Source", "classical");
  TraceCreateChannel("Source", "Destination", "classical");

  // --- Sockets ---
  // ACK/NACK: Destination <-> Source (byte[0]: 0x01=ACK, 0x00=NACK; byte[1]: attempt)
  auto srcCtrlRx = Socket::CreateSocket(src, UdpSocketFactory::GetTypeId());
  srcCtrlRx->Bind(InetSocketAddress(Ipv4Address::GetAny(), kCtrlPort));
  auto dstCtrlTx = Socket::CreateSocket(dst, UdpSocketFactory::GetTypeId());
  dstCtrlTx->Connect(InetSocketAddress(kSrcAddr, kCtrlPort));

  // Notify: Source <-> Destination 
  auto dstNotifyRx = Socket::CreateSocket(dst, UdpSocketFactory::GetTypeId());
  dstNotifyRx->Bind(InetSocketAddress(Ipv4Address::GetAny(), kNotifyPort));
  auto srcNotifyTx = Socket::CreateSocket(src, UdpSocketFactory::GetTypeId());
  srcNotifyTx->Connect(InetSocketAddress(kDstAddr, kNotifyPort));

  // --- Attempt state ---
  int attemptNum = 0;
  int dstLatestArrived = 0;
  std::shared_ptr<Qubit> currentMemQ;
  std::string currentMemLabel;

  std::function<void()> scheduleAttempt;

  // Dest Callback: notification received - arm receive-timeout | NACK on expiry
  dstNotifyRx->SetRecvCallback([&](Ptr<Socket> sock) {
    Address from;
    Ptr<Packet> pkt;
    while ((pkt = sock->RecvFrom(from))) {
      uint8_t attByte = 0;
      pkt->CopyData(&attByte, 1);
      const int att = static_cast<int>(attByte);

      std::cout << "[DEST] Notify for attempt " << att << " - arming timeout\n";
      TraceNodeText("Destination", StrCat("Expecting qubit (attempt ", att, ")"));

      Simulator::Schedule(kDstTimeout, [&, att]() {
        if (dstLatestArrived >= att) return;
        std::cout << "[DEST] Attempt " << att << " timeout - qubit lost, sending timeout signal\n";
        TraceNodeText("Destination", StrCat("Timeout: attempt ", att, " - qubit lost"));
        TraceRemoveQubit("fly_" + std::to_string(att), "lost");
        const auto tNow = Simulator::Now();
        uint8_t buf[2] = {0x00, static_cast<uint8_t>(att)};
        dstCtrlTx->Send(Create<Packet>(buf, 2));
        TraceSendPacket("Destination", "Source", tNow, tNow + kClassical,
                        StrCat("timeout: attempt ", att), "udp");
      });
    }
  });

  // Dest Callback: qubit arrived - record receipt, send ACK
  dst->SetRecvCallback([&](std::shared_ptr<Qubit> q) {
    dstLatestArrived = attemptNum;
    TraceNodeText("Destination", StrCat(q->GetLabel(), " arrived"));
    Trace("Destination received ", q->GetLabel());
    std::cout << "[DEST] " << q->GetLabel() << " arrived\n";
    const auto tNow = Simulator::Now();
    uint8_t buf[2] = {0x01, static_cast<uint8_t>(attemptNum)};
    dstCtrlTx->Send(Create<Packet>(buf, 2));
    TraceSendPacket("Destination", "Source", tNow, tNow + kClassical,
                    StrCat("ACK: ", q->GetLabel()), "udp");
  });

  // Source Callback: ACK (0x01) or NACK (0x00) from Destination
  srcCtrlRx->SetRecvCallback([&](Ptr<Socket> sock) {
    Address from;
    Ptr<Packet> pkt;
    while ((pkt = sock->RecvFrom(from))) {
      uint8_t buf[2] = {0, 0};
      pkt->CopyData(buf, 2);
      const bool isAck = (buf[0] == 0x01);
      const int  att   = static_cast<int>(buf[1]);
      if (isAck) {
        Trace("Source received ACK - entanglement established on attempt ", attemptNum);
        TraceNodeText("Source", "ACK received - entanglement distributed!");
        std::cout << "[SOURCE] ACK on attempt " << attemptNum << " - success\n";
        Simulator::Stop();
      } else {
        std::cout << "[SOURCE] Timeout for attempt " << att << " - retrying\n";
        Trace("Timeout for attempt ", att, " - qubit lost");
        TraceNodeText("Source", StrCat("Timeout: attempt ", att, " - discarding mem qubit"));
        TraceRemoveQubit(currentMemLabel, "discarded");
        currentMemQ.reset();
        Simulator::Schedule(NanoSeconds(100), scheduleAttempt);
      }
    }
  });

  // scheduleAttempt: generate Bell pair, dispatch flying qubit + notify packet.
  scheduleAttempt = [&]() {
    ++attemptNum;

    if (attemptNum > kMaxAttempts) {
      std::cout << "[SOURCE] Max attempts (" << kMaxAttempts << ") reached - giving up\n";
      TraceNodeText("Source", StrCat("Max attempts (", kMaxAttempts, ") reached - aborting"));
      Simulator::Stop();
      return;
    }

    const std::string memLabel = "mem_" + std::to_string(attemptNum);
    const std::string flyLabel = "fly_" + std::to_string(attemptNum);

    Trace("Attempt ", attemptNum, ": Source generates Bell pair (", memLabel, ", ", flyLabel, ")");
    TraceNodeText("Source", StrCat("Attempt ", attemptNum, ": generating Bell pair"));

    auto memQ = src->CreateQubit(memLabel);
    auto flyQ = src->CreateQubit(flyLabel);
    currentMemQ = memQ;
    currentMemLabel = memLabel;

    TraceCreateQubit("Source", memLabel);
    TraceCreateQubit("Source", flyLabel);

    const int thisAttempt = attemptNum;
    const uint8_t attByte = static_cast<uint8_t>(thisAttempt);

    // Step 1 (kSingleGate): H on memQ creates superposition |+>.
    Simulator::Schedule(kSingleGate, [=, &srcNotifyTx]() {
      src->Apply(gates::H(), {memQ});
      TraceNodeText("Source", StrCat("H(", memLabel, ")"));

      // Step 2 (kTwoQGate): CNOT(memQ, flyQ) entangles the pair into |Phi+>.
      Simulator::Schedule(kTwoQGate, [=, &srcNotifyTx]() {
        src->Apply(gates::CNOT(), {memQ, flyQ});
        TraceEntangle({memLabel, flyLabel}, kTwoQGate);
        TraceNodeText("Source", StrCat("Bell pair ready: ", memLabel, " + ", flyLabel));

        // Step 3 (kSingleGate): Qubit travels the quantum channel (kQDelay = 10 ns)
        // Notify packet travels P2P (10 ns propagation + ~2.5 ns tx at 100 Mbps).
        // Please note: the qubit always arrives ~2.5 ns before the notification
        // when not lost. kDstTimeout is scheduled from notification arrival.
        Simulator::Schedule(kSingleGate, [=, &srcNotifyTx]() {
          const auto tSend = Simulator::Now();

          src->Send(flyQ, dst->GetId());
          TraceSendQubit(flyLabel, "Source", "Destination", tSend, tSend + kQDelay);

          // Notification arms the receive-timeout at Destination.
          srcNotifyTx->Send(Create<Packet>(&attByte, 1));
          TraceSendPacket("Source", "Destination", tSend, tSend + kClassical,
                          StrCat("notify: attempt ", thisAttempt), "udp");

          TraceNodeText("Source", StrCat(flyLabel, " sent (attempt ", thisAttempt, ")"));
          std::cout << "[SOURCE] Attempt " << thisAttempt << ": " << flyLabel << " sent\n";
        });
      });
    });
  };

  // Start attempt 1 at t=1 us
  Simulator::Schedule(MicroSeconds(1), scheduleAttempt);

  Simulator::Stop(MilliSeconds(1));
  Simulator::Run();
  Simulator::Destroy();

  TraceWriter::Instance().Close();
  std::cout << "[DONE] Channel-loss example finished\n";
  return 0;
}
