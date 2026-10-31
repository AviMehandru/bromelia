// swift-tools-version: 6.0
// BromeliaKit: the engine's layers as one target each (rearchitecture plan §4). A target can import only the
// layers below it, so an import that breaks the layer rules doesn't compile.
import PackageDescription

let package = Package(
    name: "BromeliaKit",
    platforms: [.macOS(.v14)],
    products: [
        .library(name: "BroFoundation", targets: ["BroFoundation"]),
        .library(name: "BroDomain", targets: ["BroDomain"]),
        .library(name: "BroPorts", targets: ["BroPorts"]),
        .library(name: "BroAdapters", targets: ["BroAdapters"]),
        .library(name: "BroEngine", targets: ["BroEngine"]),
    ],
    targets: [
        .target(name: "BroFoundation"),
        .target(name: "BroDomain", dependencies: ["BroFoundation"]),
        .target(name: "BroPorts", dependencies: ["BroFoundation", "BroDomain"]),
        .target(name: "BroAdapters", dependencies: ["BroFoundation", "BroDomain", "BroPorts"]),
        .target(name: "BroEngine", dependencies: ["BroFoundation", "BroDomain", "BroPorts"]),
        // Reads shared/fixtures for the tests; not part of any layer.
        .target(name: "BroTestSupport", dependencies: ["BroFoundation"], path: "Tests/BroTestSupport"),
        // Renders messages in English with the host's MessageTextRenderer, for the tests that check a text.
        .target(name: "BroTestEnglish", dependencies: ["BroFoundation", "BroAdapters"], path: "Tests/BroTestEnglish"),
        // The test kit (plan §17): fakes of the ports for the adapter and engine tests and the test composition root.
        .target(name: "BroTestKit", dependencies: ["BroFoundation", "BroDomain", "BroPorts"], path: "Tests/BroTestKit"),
        .testTarget(name: "BroFoundationTests", dependencies: ["BroFoundation", "BroTestSupport"]),
        .testTarget(name: "BroDomainTests", dependencies: ["BroDomain", "BroTestSupport", "BroTestEnglish"]),
        .testTarget(name: "BroPortsTests", dependencies: ["BroPorts", "BroTestSupport"]),
        // Started by BroAdaptersTests (CrashTests): kills itself at a chosen point. Not shipped.
        .executableTarget(name: "BroTestProbe", dependencies: ["BroFoundation", "BroDomain", "BroPorts", "BroAdapters"],
                          path: "Tests/BroTestProbe"),
        .testTarget(name: "BroAdaptersTests", dependencies: ["BroAdapters", "BroTestSupport", "BroTestEnglish", "BroTestKit", "BroTestProbe"]),
        .testTarget(name: "BroEngineTests", dependencies: ["BroEngine", "BroTestSupport", "BroTestKit"]),
        // The scenario test unit (plan §5.1): the test composition root over the real engine and adapters, and the
        // LocalEngine cases; H adds the scenario runner.
        .testTarget(name: "SharedScenarioTests", dependencies: ["BroEngine", "BroAdapters", "BroTestKit", "BroTestSupport"]),
    ],
    swiftLanguageModes: [.v6]
)
