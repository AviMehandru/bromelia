import BroDomain
import BroFoundation

/// bromeliad as a login item / service.
public protocol ServiceInstaller: Sendable {
    func install() throws(BroError)

    func uninstall() throws(BroError)

    func isInstalled() -> Bool

    func start() throws(BroError)
}
