import { render, screen, waitFor } from "@testing-library/react";
import {
  createConnectedMockZMKApp,
  createMockZMKApp,
  ZMKAppProvider,
} from "@cormoran/zmk-studio-react-hook/testing";
import {
  IncidentsSection,
  SUBSYSTEM_IDENTIFIER,
} from "../src/IncidentsSection";

describe("IncidentsSection Component", () => {
  describe("With Subsystem", () => {
    it("should render the status card and attempt to load data", async () => {
      const mockZMKApp = createConnectedMockZMKApp({
        deviceName: "Test Device",
        subsystems: [SUBSYSTEM_IDENTIFIER],
      });

      render(
        <ZMKAppProvider value={mockZMKApp}>
          <IncidentsSection />
        </ZMKAppProvider>
      );

      expect(
        screen.getByRole("heading", { name: "Status" })
      ).toBeInTheDocument();

      // The mock connection has no working transport, so the RPC call
      // rejects and the component should surface that as an error rather
      // than crash.
      await waitFor(() => {
        expect(screen.getByText(/🚨/i)).toBeInTheDocument();
      });
    });

    it("should render an empty incidents table before any data loads", () => {
      const mockZMKApp = createConnectedMockZMKApp({
        subsystems: [SUBSYSTEM_IDENTIFIER],
      });

      render(
        <ZMKAppProvider value={mockZMKApp}>
          <IncidentsSection />
        </ZMKAppProvider>
      );

      expect(screen.getByText(/Incidents \(0\)/i)).toBeInTheDocument();
      expect(screen.getByText(/No incidents recorded/i)).toBeInTheDocument();
    });
  });

  describe("Without Subsystem", () => {
    it("should show warning when subsystem is not found", () => {
      const mockZMKApp = createConnectedMockZMKApp({
        deviceName: "Test Device",
        subsystems: [],
      });

      render(
        <ZMKAppProvider value={mockZMKApp}>
          <IncidentsSection />
        </ZMKAppProvider>
      );

      expect(
        screen.getByText(/Subsystem "cormoran__watchdog" not found/i)
      ).toBeInTheDocument();
      expect(
        screen.getByText(
          /Make sure your firmware includes the watchdog module/i
        )
      ).toBeInTheDocument();
    });
  });

  describe("Without ZMKAppContext", () => {
    it("should not render when ZMKAppContext is not provided", () => {
      const { container } = render(<IncidentsSection />);

      expect(container.firstChild).toBeNull();
    });
  });

  describe("Disconnected app", () => {
    it("should not attempt to load data without a connection", () => {
      const mockZMKApp = createMockZMKApp({
        findSubsystem: () => ({ index: 0, identifier: SUBSYSTEM_IDENTIFIER }),
      });

      render(
        <ZMKAppProvider value={mockZMKApp}>
          <IncidentsSection />
        </ZMKAppProvider>
      );

      expect(
        screen.getByRole("heading", { name: "Status" })
      ).toBeInTheDocument();
      expect(screen.getByText(/No status loaded yet/i)).toBeInTheDocument();
    });
  });
});
