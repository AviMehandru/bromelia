import BroDomain
import BroFoundation

/// Engine-side notifications, used when no app is connected.
public protocol DesktopNotifier: Sendable {
    func post(_ title: String, body: String, sound: Bool)
}
