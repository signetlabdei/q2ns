/*-----------------------------------------------------------------------------
 * Q2NS - Quantum Network Simulator
 * Copyright (c) 2026 quantuminternet.it
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *---------------------------------------------------------------------------*/
/**
 * @file q2nsviz-repeater-swap-example.cc
 * @brief Entanglement swapping via a quantum repeater (source-centric BSG model).
 *
 * Two dedicated Bell-State Generator (BSG) source nodes create the link-level
 * entanglement and distribute the qubits:
 *   Src1 generates (fly_a, rep_a): fly_a -> Alice, rep_a -> Repeater.
 *   Src2 generates (rep_b, fly_b): rep_b -> Repeater, fly_b -> Bob.
 *
 * Once Alice and Bob receive their flying qubits they each send a classical ACK
 * to the Repeater. When both ACKs arrive the Repeater performs a Bell-State
 * Measurement (BSM) on mem_a and mem_b, swapping entanglement end-to-end.
 * BSM outcomes are forwarded to Bob, who applies the Pauli corrections.
 *
 * Timing model (illustrative):
 *   kSingleGate = 100 ns  (single-qubit gate)
 *   kTwoQGate   = 300 ns  (two-qubit gate / BSM)
 *   kQDelay     =  10 ns  (quantum channel propagation, ~2 m fiber)
 *
 * Visualization output is written to
 * examples/example_traces/q2nsviz-repeater-swap-example.json and can be loaded in the
 * q2nsviz viewer (src/q2ns/utils/q2nsviz-serve.sh).
 */

#include "ns3/core-module.h"
#include "ns3/internet-module.h"
#include "ns3/network-module.h"
#include "ns3/point-to-point-module.h"
#include "ns3/simulator.h"

#include "ns3/q2ns-netcontroller.h"
#include "ns3/q2ns-qgate.h"
#include "ns3/q2ns-qnode.h"
#include "ns3/q2ns-qubit.h"

#include "ns3/q2nsviz-trace-writer.h"
#include "ns3/q2nsviz-trace.h"

#include <iostream>

using namespace ns3;
using namespace q2ns;

static const uint16_t kAckPort  = 9200; // Repeater listens for arrival ACKs
static const uint16_t kCtrlPort = 9100; // Bob listens for BSM corrections

int main(int argc, char** argv) {
  std::cout << "[DEMO] Repeater-centric entanglement swap starting\n";

  RngSeedManager::SetSeed(1);
  RngSeedManager::SetRun(1);

  CommandLine cmd;
  cmd.Parse(argc, argv);

  TraceWriter::Instance().Open(Q2NS_EXAMPLE_TRACES_DIR "q2nsviz-repeater-swap-example.json");
  Trace("Repeater-Centric Entanglement Swap");

  Time::SetResolution(Time::NS);

  NetController net;

  // --- Nodes ---
  auto alice    = net.CreateNode();
  auto src1     = net.CreateNode();
  auto repeater = net.CreateNode();
  auto src2     = net.CreateNode();
  auto bob      = net.CreateNode();

  TraceCreateNode("Alice",    10, 50);
  TraceCreateNode("Src1",     30, 30);
  TraceCreateNode("Repeater", 50, 50);
  TraceCreateNode("Src2",     70, 30);
  TraceCreateNode("Bob",      90, 50);

  // --- Timing constants ---
  const Time kSingleGate = NanoSeconds(100);
  const Time kTwoQGate   = NanoSeconds(300);
  const Time kQDelay     = NanoSeconds(10);
  const Time kClassical  = kQDelay;

  // --- Quantum links (Src1 distributes to Alice and Repeater; Src2 to Repeater and Bob) ---
  auto chS1A = net.InstallQuantumLink(src1, alice);
  auto chS1R = net.InstallQuantumLink(src1, repeater);
  auto chS2R = net.InstallQuantumLink(src2, repeater);
  auto chS2B = net.InstallQuantumLink(src2, bob);
  chS1A->SetAttribute("Delay", TimeValue(kQDelay));
  chS1R->SetAttribute("Delay", TimeValue(kQDelay));
  chS2R->SetAttribute("Delay", TimeValue(kQDelay));
  chS2B->SetAttribute("Delay", TimeValue(kQDelay));

  TraceCreateChannel("Src1",     "Alice",    "quantum");
  TraceCreateChannel("Src1",     "Repeater", "quantum");
  TraceCreateChannel("Src2",     "Repeater", "quantum");
  TraceCreateChannel("Src2",     "Bob",      "quantum");

  // --- Classical network (Alice, Repeater, Bob only; Src1 and Src2 are quantum-only) ---
  InternetStackHelper internet;
  internet.Install(alice);
  internet.Install(repeater);
  internet.Install(bob);

  PointToPointHelper p2p;
  p2p.SetDeviceAttribute("DataRate", StringValue("100Mbps"));
  p2p.SetChannelAttribute("Delay", StringValue("10ns"));

  Ipv4AddressHelper ipv4;
  ipv4.SetBase("10.1.1.0", "255.255.255.0");
  auto ifsAR = ipv4.Assign(p2p.Install(alice, repeater));
  ipv4.SetBase("10.1.2.0", "255.255.255.0");
  auto ifsRB = ipv4.Assign(p2p.Install(repeater, bob));
  Ipv4GlobalRoutingHelper::PopulateRoutingTables();

  const Ipv4Address kRepeaterAddr = ifsAR.GetAddress(1);
  const Ipv4Address kBobAddr      = ifsRB.GetAddress(1);

  TraceCreateChannel("Alice",    "Repeater", "classical");
  TraceCreateChannel("Bob",      "Repeater", "classical");
  TraceCreateChannel("Repeater", "Bob",      "classical");

  // --- Sockets ---
  auto repeaterAckRx = Socket::CreateSocket(repeater, UdpSocketFactory::GetTypeId());
  repeaterAckRx->Bind(InetSocketAddress(Ipv4Address::GetAny(), kAckPort));

  auto aliceTx = Socket::CreateSocket(alice, UdpSocketFactory::GetTypeId());
  aliceTx->Connect(InetSocketAddress(kRepeaterAddr, kAckPort));

  auto bobAckTx = Socket::CreateSocket(bob, UdpSocketFactory::GetTypeId());
  bobAckTx->Connect(InetSocketAddress(kRepeaterAddr, kAckPort));

  auto repeaterTx = Socket::CreateSocket(repeater, UdpSocketFactory::GetTypeId());
  repeaterTx->Connect(InetSocketAddress(kBobAddr, kCtrlPort));

  auto bobCtrlRx = Socket::CreateSocket(bob, UdpSocketFactory::GetTypeId());
  bobCtrlRx->Bind(InetSocketAddress(Ipv4Address::GetAny(), kCtrlPort));

  // --- Shared state ---
  int  acksReceived  = 0;
  bool bsmDone       = false;

  // Repeater memory qubits (filled by quantum recv callback)
  std::shared_ptr<Qubit> repMem1;
  std::shared_ptr<Qubit> repMem2;
  int repQubitsRxd = 0;

  // Repeater: receive mem_a from Src1 and mem_b from Src2
  repeater->SetRecvCallback([&](std::shared_ptr<Qubit> q) {
    ++repQubitsRxd;
    if (repQubitsRxd == 1) {
      repMem1 = q;
      TraceNodeText("Repeater", StrCat(q->GetLabel(), " arrived (1/2)"));
      std::cout << "[REPEATER][quantum] " << q->GetLabel() << " arrived (1/2)\n";
    } else {
      repMem2 = q;
      TraceNodeText("Repeater", StrCat(q->GetLabel(), " arrived (2/2) - ready for BSM"));
      std::cout << "[REPEATER][quantum] " << q->GetLabel() << " arrived (2/2)\n";
    }
  });

  struct BobCtx {
    std::shared_ptr<Qubit> qFlyB;
    bool qubitArrived = false;
    bool bitsArrived  = false;
    int m1 = 0;
    int m2 = 0;
  };
  BobCtx bobCtx;

  auto tryBobCorrections = [&]() {
    if (!bobCtx.qubitArrived || !bobCtx.bitsArrived)
      return;
    Trace("Bob applies Pauli corrections X^m2=", bobCtx.m2, " Z^m1=", bobCtx.m1);
    if (bobCtx.m2) {
      bob->Apply(gates::X(), {bobCtx.qFlyB});
      TraceNodeText("Bob", "X correction applied (m2=1)");
    } else {
      TraceNodeText("Bob", "X correction skipped (m2=0)");
    }
    if (bobCtx.m1) {
      bob->Apply(gates::Z(), {bobCtx.qFlyB});
      TraceNodeText("Bob", "Z correction applied (m1=1)");
    } else {
      TraceNodeText("Bob", "Z correction skipped (m1=0)");
    }
    TraceRemoveCbit("m1");
    TraceRemoveCbit("m2");
    Trace("End-to-end entanglement established between Alice and Bob");
    std::cout << "[DONE] End-to-end entanglement established\n";
    Simulator::Stop();
  };

  // Alice: flying qubit arrived -> send ACK to Repeater
  alice->SetRecvCallback([&](std::shared_ptr<Qubit> q) {
    TraceNodeText("Alice", StrCat(q->GetLabel(), " arrived"));
    std::cout << "[ALICE][quantum] " << q->GetLabel() << " arrived at t="
              << Simulator::Now().GetNanoSeconds() << " ns\n";
    aliceTx->Send(Create<Packet>(1));
    const auto tNow = Simulator::Now();
    TraceSendPacket("Alice", "Repeater", tNow, tNow + kClassical, "ACK: fly_a received", "udp");
  });

  // Bob: flying qubit arrived -> send ACK to Repeater; store qubit for corrections
  bob->SetRecvCallback([&](std::shared_ptr<Qubit> q) {
    bobCtx.qFlyB       = q;
    bobCtx.qubitArrived = true;
    TraceNodeText("Bob", StrCat(q->GetLabel(), " arrived - awaiting BSM corrections"));
    std::cout << "[BOB][quantum] " << q->GetLabel() << " arrived at t="
              << Simulator::Now().GetNanoSeconds() << " ns\n";
    bobAckTx->Send(Create<Packet>(1));
    const auto tNow = Simulator::Now();
    TraceSendPacket("Bob", "Repeater", tNow, tNow + kClassical, "ACK: fly_b received", "udp");
    tryBobCorrections();
  });

  // Bob: classical corrections from Repeater
  bobCtrlRx->SetRecvCallback([&](Ptr<Socket> sock) {
    Address from;
    while (Ptr<Packet> p = sock->RecvFrom(from)) {
      uint8_t bits[2] = {0, 0};
      p->CopyData(bits, 2);
      bobCtx.m1        = bits[0] & 1;
      bobCtx.m2        = bits[1] & 1;
      bobCtx.bitsArrived = true;
      Trace("Bob receives corrections m1=", bobCtx.m1, " m2=", bobCtx.m2);
      TraceNodeText("Bob", StrCat("Corrections received: m1=", bobCtx.m1, " m2=", bobCtx.m2));
      std::cout << "[BOB][CTRL] corrections m1=" << bobCtx.m1 << " m2=" << bobCtx.m2 << "\n";
      tryBobCorrections();
    }
  });

  // Repeater: both ACKs received -> perform BSM and forward outcomes to Bob
  repeaterAckRx->SetRecvCallback([&](Ptr<Socket> sock) {
    Address from;
    while (sock->RecvFrom(from)) {
      ++acksReceived;
      TraceNodeText("Repeater", StrCat("ACK received (", acksReceived, "/2)"));
      std::cout << "[REPEATER] ACK " << acksReceived << "/2 received\n";

      if (acksReceived == 2 && !bsmDone) {
        bsmDone = true;

        if (!repMem1 || !repMem2) {
          std::cerr << "[REPEATER] ERROR: memory qubits not ready for BSM!\n";
          return;
        }

        Trace("Repeater performs BSM on rep_a and rep_b");
        TraceNodeText("Repeater", "BSM: measuring rep_a and rep_b");
        auto [m1, m2] = repeater->MeasureBell(repMem1, repMem2);
        std::cout << "[REPEATER][BSM] m1=" << m1 << " m2=" << m2 << "\n";

        TraceMeasure("rep_a", kTwoQGate, "Bell");
        TraceMeasure("rep_b", kTwoQGate, "Bell");
        TraceEntangle({"fly_a", "fly_b"});
        TraceRemoveQubit("rep_a", "discarded");
        TraceRemoveQubit("rep_b", "discarded");

        const auto tNow = Simulator::Now();
        uint8_t bits[2] = {(uint8_t)(m1 & 1), (uint8_t)(m2 & 1)};
        repeaterTx->Send(Create<Packet>(bits, 2));

        TraceCreateCbit("Repeater", "m1");
        TraceCreateCbit("Repeater", "m2");
        TraceSendCbit("m1", "Repeater", "Bob", tNow, tNow + kClassical);
        TraceSendCbit("m2", "Repeater", "Bob", tNow, tNow + kClassical);
        TraceSendPacket("Repeater", "Bob", tNow, tNow + kClassical,
                        StrCat("BSM corrections: m1=", m1, " m2=", m2), "udp");
        TraceNodeText("Repeater", StrCat("Corrections m1=", m1, " m2=", m2, " sent to Bob"));
      }
    }
  });

  // Src1 and Src2 each generate a Bell pair and distribute qubits
  Simulator::Schedule(MicroSeconds(1), [&]() {
    Trace("Phase 1: Src1 and Src2 generate Bell pairs");
    TraceNodeText("Src1", "Generating Bell pair (fly_a, rep_a)");
    TraceNodeText("Src2", "Generating Bell pair (rep_b, fly_b)");

    auto [flyA, memA] = src1->CreateBellPair();
    flyA->SetLabel("fly_a");
    memA->SetLabel("rep_a");
    TraceCreateQubit("Src1", "fly_a");
    TraceCreateQubit("Src1", "rep_a");

    auto [memB, flyB] = src2->CreateBellPair();
    memB->SetLabel("rep_b");
    flyB->SetLabel("fly_b");
    TraceCreateQubit("Src2", "rep_b");
    TraceCreateQubit("Src2", "fly_b");

    Simulator::Schedule(kTwoQGate, [&, flyA, memA, memB, flyB]() {
      TraceEntangle({"fly_a", "rep_a"}, kTwoQGate);
      TraceEntangle({"rep_b", "fly_b"}, kTwoQGate);
      TraceNodeText("Src1", "|Phi+>_S1 ready (fly_a, rep_a)");
      TraceNodeText("Src2", "|Phi+>_S2 ready (rep_b, fly_b)");
      std::cout << "[SRC1] Bell pair ready\n";
      std::cout << "[SRC2] Bell pair ready\n";

      // Distribute: fly_a -> Alice, mem_a -> Repeater, mem_b -> Repeater, fly_b -> Bob
      Simulator::Schedule(kSingleGate, [&, flyA, memA, memB, flyB]() {
        Trace("Phase 2: distributing qubits to endpoints and repeater");
        const auto tSend = Simulator::Now();

        src1->Send(flyA, alice->GetId());
        TraceSendQubit("fly_a", "Src1", "Alice",    tSend, tSend + kQDelay);

        src1->Send(memA, repeater->GetId());
        TraceSendQubit("rep_a", "Src1", "Repeater", tSend, tSend + kQDelay);

        src2->Send(memB, repeater->GetId());
        TraceSendQubit("rep_b", "Src2", "Repeater", tSend, tSend + kQDelay);

        src2->Send(flyB, bob->GetId());
        TraceSendQubit("fly_b", "Src2", "Bob",      tSend, tSend + kQDelay);

        TraceNodeText("Src1", "fly_a -> Alice, rep_a -> Repeater");
        TraceNodeText("Src2", "rep_b -> Repeater, fly_b -> Bob");
        std::cout << "[SRC1] fly_a sent to Alice, rep_a sent to Repeater\n";
        std::cout << "[SRC2] rep_b sent to Repeater, fly_b sent to Bob\n";
      });
    });
  });

  Simulator::Stop(MilliSeconds(1));
  Simulator::Run();
  Simulator::Destroy();

  TraceWriter::Instance().Close();
  std::cout << "[DONE] Repeater swap finished\n";
  return 0;
}
