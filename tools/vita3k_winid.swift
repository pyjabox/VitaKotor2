// vita3k_winid.swift -- prints the CoreGraphics window number of Vita3K's game
// window (the one titled after the running app), for screencapture -l.
// Build: swiftc -O tools/vita3k_winid.swift -o <somewhere>/vita3k_winid
import CoreGraphics
let opts = CGWindowListOption(arrayLiteral: .optionOnScreenOnly, .excludeDesktopElements)
if let list = CGWindowListCopyWindowInfo(opts, kCGNullWindowID) as? [[String: Any]] {
  for w in list {
    let owner = w[kCGWindowOwnerName as String] as? String ?? ""
    let name = w[kCGWindowName as String] as? String ?? ""
    if owner == "Vita3K" && name.hasPrefix("KOTOR II") {
      print(w[kCGWindowNumber as String] as? Int ?? 0)
      break
    }
  }
}
