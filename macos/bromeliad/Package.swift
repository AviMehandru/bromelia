// swift-tools-version: 6.0
// bromeliad: the macOS host (rearchitecture plan §4 Hosts, §17). Phase 3 builds the real
// composition root as a library; phase 4 makes this target the bromeliad executable (main, HostOptions, EngineLock).
import PackageDescription

let package = Package(
    name: "bromeliad",
    platforms: [.macOS(.v14)],
    products: [
        .library(name: "Bromeliad", targets: ["Bromeliad"]),
    ],
    dependencies: [
        .package(path: "../BromeliaKit"),
    ],
    targets: [
        .target(name: "Bromeliad",
                dependencies: [
                    .product(name: "BroFoundation", package: "BromeliaKit"),
                    .product(name: "BroDomain", package: "BromeliaKit"),
                    .product(name: "BroPorts", package: "BromeliaKit"),
                    .product(name: "BroAdapters", package: "BromeliaKit"),
                    .product(name: "BroEngine", package: "BromeliaKit"),
                ],
                path: ".", exclude: ["Tests"]),
        // The real root's smoke test.
        .testTarget(name: "BromeliadTests", dependencies: ["Bromeliad"], path: "Tests/BromeliadTests"),
    ],
    swiftLanguageModes: [.v6]
)
