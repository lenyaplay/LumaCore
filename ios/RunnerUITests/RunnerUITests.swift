import XCTest

// Temporary verification-only UI test for docs/ai_plans/02-ios-recording-swift-flutter-integration.md
// steps 6-7 (live device recording + Photos save). Not meant to remain as a
// permanent regression test — remove after the manual verification pass.
final class RunnerUITests: XCTestCase {
  override func setUpWithError() throws {
    continueAfterFailure = false
  }

  func testRecordingFlow() throws {
    let app = XCUIApplication()

    addUIInterruptionMonitor(withDescription: "System permission alert") { alert in
      let allowLabels = ["Allow", "OK", "Allow Once", "Allow While Using App", "Разрешить", "ОК"]
      for label in allowLabels {
        let button = alert.buttons[label]
        if button.exists {
          button.tap()
          return true
        }
      }
      return false
    }

    app.launch()

    let licenseField = app.textFields.firstMatch
    XCTAssertTrue(licenseField.waitForExistence(timeout: 10), "license text field never appeared")
    licenseField.tap()
    licenseField.typeText("TEST-LICENSE-KEY")

    let activateButton = app.buttons["Activate"]
    XCTAssertTrue(activateButton.waitForExistence(timeout: 5), "Activate button never appeared")
    activateButton.tap()

    // Poke a neutral corner to force XCTest to evaluate the interruption
    // monitor for the camera-permission alert, which can appear as soon as
    // the camera screen starts capture.
    pokeToDismissInterruptions(app)

    let recordButton = app.buttons["Start Recording"]
    XCTAssertTrue(recordButton.waitForExistence(timeout: 15), "record button never appeared")
    recordButton.tap()

    Thread.sleep(forTimeInterval: 3)

    let stopButton = app.buttons["Stop Recording"]
    XCTAssertTrue(stopButton.waitForExistence(timeout: 5), "stop button never appeared")
    stopButton.tap()

    // Photos add-only permission alert appears during stop()'s save; give it
    // time to show and be auto-dismissed.
    for _ in 0..<5 {
      pokeToDismissInterruptions(app)
      Thread.sleep(forTimeInterval: 1)
    }

    let screenshot = app.screenshot()
    let attachment = XCTAttachment(screenshot: screenshot)
    attachment.lifetime = .keepAlways
    attachment.name = "final-state"
    add(attachment)
  }

  private func pokeToDismissInterruptions(_ app: XCUIApplication) {
    app.coordinate(withNormalizedOffset: CGVector(dx: 0.02, dy: 0.02)).tap()
  }
}
