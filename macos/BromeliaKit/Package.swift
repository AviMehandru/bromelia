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
    ],
    targets: [
        .target(name: "BroFoundation"),
        .target(name: "BroDomain", dependencies: ["BroFoundation"]),
        .target(name: "BroPorts", dependencies: ["BroFoundation", "BroDomain"]),
        .target(name: "BroAdapters", dependencies: ["BroFoundation", "BroDomain", "BroPorts"]),
        // Reads shared/fixtures for the tests; not part of any layer.
        .target(name: "BroTestSupport", dependencies: ["BroFoundation"], path: "Tests/BroTestSupport"),
        .testTarget(name: "BroFoundationTests", dependencies: ["BroFoundation", "BroTestSupport"]),
        .testTarget(name: "BroDomainTests", dependencies: ["BroDomain", "BroTestSupport"]),
        .testTarget(name: "BroPortsTests", dependencies: ["BroPorts", "BroTestSupport"]),
        // Started by BroAdaptersTests (CrashTests): kills itself at a chosen point. Not shipped.
        .executableTarget(name: "BroTestProbe", dependencies: ["BroFoundation", "BroDomain", "BroPorts", "BroAdapters"],
                          path: "Tests/BroTestProbe"),
        .testTarget(name: "BroAdaptersTests", dependencies: ["BroAdapters", "BroTestSupport", "BroTestProbe"]),
    ],
    swiftLanguageModes: [.v6]
)
