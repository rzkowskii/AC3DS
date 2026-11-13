import AppKit
import CoreGraphics
import Foundation

func fail(_ message: String) -> Never {
    FileHandle.standardError.write(Data((message + "\n").utf8))
    exit(1)
}

let arguments = Array(CommandLine.arguments.dropFirst())
guard arguments.count >= 3,
      let pid = pid_t(arguments[0]), pid > 0,
      let app = NSRunningApplication(processIdentifier: pid),
      app.executableURL?.lastPathComponent.lowercased() == "azahar"
else {
    fail("Usage: azahar-keys AZAHAR_PID KEY SECONDS [KEY SECONDS ...]. Use WAIT to pause.")
}
guard CGPreflightPostEventAccess() else { fail("macOS denied input injection access") }
guard let source = CGEventSource(stateID: .privateState) else { fail("Could not create an event source") }

let keys: [String: CGKeyCode] = [
    "A": 0, "S": 1, "D": 2, "F": 3, "H": 4, "G": 5, "Z": 6, "X": 7,
    "Q": 12, "W": 13, "E": 14, "T": 17, "1": 18, "2": 19, "O": 31, "I": 34,
    "L": 37, "J": 38, "K": 40, "N": 45, "M": 46,
    "LEFT": 123, "RIGHT": 124, "DOWN": 125, "UP": 126
]
let actionArguments = Array(arguments.dropFirst())
guard actionArguments.count.isMultiple(of: 2) else { fail("Each key needs a duration") }

var actions = [(name: String, code: CGKeyCode?, seconds: Double)]()
for index in stride(from: 0, to: actionArguments.count, by: 2) {
    let name = actionArguments[index].uppercased()
    guard let seconds = Double(actionArguments[index + 1]), (0.05...4).contains(seconds) else {
        fail("Each duration must be 0.05–4 seconds")
    }
    let code = keys[name]
    guard code != nil || name == "WAIT" else { fail("Unknown host key: \(name)") }
    actions.append((name, code, seconds))
}
guard actions.reduce(0, { $0 + $1.seconds }) <= 16 else {
    fail("Use at most 16 seconds of total input")
}

func post(_ code: CGKeyCode, down: Bool) {
    guard !app.isTerminated,
          let event = CGEvent(keyboardEventSource: source, virtualKey: code, keyDown: down)
    else { fail("The owned Azahar process or event is unavailable") }
    event.flags = []
    event.postToPid(pid)
}

guard app.isActive || app.activate() else { fail("Could not activate the owned Azahar process") }
Thread.sleep(forTimeInterval: 0.25)
for (name, code, seconds) in actions {
    if let code { post(code, down: true) }
    Thread.sleep(forTimeInterval: seconds)
    if let code {
        post(code, down: false)
        print("posted \(name) for \(seconds)s")
    }
}
