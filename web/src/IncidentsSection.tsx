import { useContext, useEffect, useState, useCallback } from "react";
import {
  ZMKCustomSubsystem,
  ZMKAppContext,
} from "@cormoran/zmk-studio-react-hook";
import {
  Request,
  Response,
  Incident,
  Notification,
} from "./proto/cormoran/watchdog/watchdog";
import {
  incidentDetailSummary,
  incidentTypeLabel,
  sourceLabel,
} from "./incidentHelpers";

export const SUBSYSTEM_IDENTIFIER = "cormoran__watchdog";

const PAGE_SIZE = 4;

// How long to wait for a peripheral to answer a relayed request (DESIGN.md
// SS7) before giving up -- there is no way to know which peripheral slots
// are currently connected, so a relayed request to a disconnected/missing
// peripheral would otherwise hang forever.
const RELAY_TIMEOUT_MS = 3000;

interface Status {
  capacity: number;
  stored: number;
  droppedSinceBoot: number;
  recordingStopped: boolean;
}

export function IncidentsSection() {
  const zmkApp = useContext(ZMKAppContext);
  // 0 = central/local (the only source in Phase D); >0 = split peripheral
  // slot N, relayed over CONFIG_ZMK_SPLIT_RELAY_EVENT (DESIGN.md SS7). There
  // is no discovery of how many peripherals are connected, so this offers a
  // small fixed range -- enough for the common single/dual-peripheral split.
  const [source, setSource] = useState(0);
  const [status, setStatus] = useState<Status | null>(null);
  const [incidents, setIncidents] = useState<Incident[]>([]);
  const [isLoading, setIsLoading] = useState(false);
  const [error, setError] = useState<string | null>(null);
  const [confirmingDeleteAll, setConfirmingDeleteAll] = useState(false);

  const subsystem = zmkApp?.findSubsystem(SUBSYSTEM_IDENTIFIER) ?? null;

  const callRpc = useCallback(
    async (request: Request): Promise<Response | null> => {
      if (!zmkApp?.state.connection || !subsystem) return null;
      const service = new ZMKCustomSubsystem(
        zmkApp.state.connection,
        subsystem.index
      );
      const payload = Request.encode(request).finish();
      const responsePayload = await service.callRPC(payload);
      if (!responsePayload) return null;
      return Response.decode(responsePayload);
    },
    [zmkApp, subsystem]
  );

  // Awaits the real Response for a request that came back as a
  // DeferredResponse (source != 0, see DESIGN.md SS7): subscribes to
  // PeripheralResponse notifications and resolves on the first one matching
  // requestId, or null after RELAY_TIMEOUT_MS with no answer (peripheral
  // disconnected/absent -- a documented v1 limitation, there is no way to
  // detect this ahead of time).
  const awaitPeripheralResponse = useCallback(
    (requestId: number): Promise<Response | null> => {
      return new Promise((resolve) => {
        if (!zmkApp || !subsystem) {
          resolve(null);
          return;
        }
        let settled = false;
        const finish = (value: Response | null) => {
          if (settled) return;
          settled = true;
          clearTimeout(timer);
          unsubscribe();
          resolve(value);
        };
        const unsubscribe = zmkApp.onNotification({
          type: "custom",
          subsystemIndex: subsystem.index,
          callback: (customNotification) => {
            let notification: Notification;
            try {
              notification = Notification.decode(customNotification.payload);
            } catch {
              return;
            }
            const pr = notification.peripheralResponse;
            if (pr && pr.requestId === requestId) {
              finish(pr.response ?? null);
            }
          },
        });
        const timer = setTimeout(() => finish(null), RELAY_TIMEOUT_MS);
      });
    },
    [zmkApp, subsystem]
  );

  // Calls the RPC and, if it came back as a DeferredResponse (relayed to a
  // split peripheral), waits for the real answer instead. Returns null (and
  // sets an error) on timeout.
  const callRpcAwaitingRelay = useCallback(
    async (request: Request): Promise<Response | null> => {
      const resp = await callRpc(request);
      if (!resp?.deferred) return resp;
      const relayed = await awaitPeripheralResponse(resp.deferred.requestId);
      if (!relayed) {
        setError(
          `Peripheral ${source} did not respond (timed out after ${RELAY_TIMEOUT_MS}ms). It may be disconnected.`
        );
      }
      return relayed;
    },
    [callRpc, awaitPeripheralResponse, source]
  );

  const refreshStatus = useCallback(async () => {
    const resp = await callRpcAwaitingRelay(
      Request.create({ getStatus: { source } })
    );
    if (resp?.status) {
      setStatus({
        capacity: resp.status.capacity,
        stored: resp.status.stored,
        droppedSinceBoot: resp.status.droppedSinceBoot,
        recordingStopped: resp.status.recordingStopped,
      });
    } else if (resp?.error) {
      setError(resp.error.message);
    }
  }, [callRpcAwaitingRelay, source]);

  const refreshIncidents = useCallback(async () => {
    const collected: Incident[] = [];
    let startIndex = 0;
    // Fetch all pages on connect/refresh -- the store is capped (default 16
    // incidents), so this is at most a handful of round-trips.
    for (let guard = 0; guard < 64; guard++) {
      const resp = await callRpcAwaitingRelay(
        Request.create({ listIncidents: { startIndex, source } })
      );
      if (!resp?.incidentPage) {
        if (resp?.error) {
          setError(resp.error.message);
        }
        break;
      }
      collected.push(...resp.incidentPage.incidents);
      startIndex += resp.incidentPage.incidents.length;
      if (
        resp.incidentPage.incidents.length === 0 ||
        startIndex >= resp.incidentPage.total
      ) {
        break;
      }
    }
    setIncidents(collected);
  }, [callRpcAwaitingRelay, source]);

  const refreshAll = useCallback(async () => {
    setIsLoading(true);
    setError(null);
    try {
      await refreshStatus();
      await refreshIncidents();
    } catch (err) {
      setError(err instanceof Error ? err.message : "Failed to load incidents");
    } finally {
      setIsLoading(false);
    }
  }, [refreshStatus, refreshIncidents]);

  useEffect(() => {
    if (!subsystem || !zmkApp?.state.connection) {
      return;
    }

    let cancelled = false;

    const load = async () => {
      if (!cancelled) {
        await refreshAll();
      }
    };

    void load();

    return () => {
      cancelled = true;
    };
    // Re-run when the connected watchdog subsystem becomes available, or
    // when the user switches source (central/peripheral).
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [subsystem?.index, zmkApp?.state.connection, source]);

  useEffect(() => {
    if (!zmkApp || !subsystem) return;

    const unsubscribe = zmkApp.onNotification({
      type: "custom",
      subsystemIndex: subsystem.index,
      callback: (customNotification) => {
        let notification: Notification;
        try {
          notification = Notification.decode(customNotification.payload);
        } catch {
          return;
        }
        // Only IncidentRecorded is a live/spontaneous push -- it always
        // describes this device's own local store (source 0), matching
        // Phase D behavior unchanged. PeripheralResponse notifications are
        // consumed by awaitPeripheralResponse() above (they are answers to
        // a specific requestId, not a general live-update stream) and
        // intentionally ignored here.
        if (notification.incidentRecorded?.incident) {
          const incident = notification.incidentRecorded.incident;
          if (source !== 0) return;
          setIncidents((prev) => [
            incident,
            ...prev.filter((i) => i.id !== incident.id),
          ]);
          refreshStatus();
        }
      },
    });

    return unsubscribe;
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [zmkApp, subsystem?.index, source]);

  const deleteOne = async (id: number) => {
    setIsLoading(true);
    setError(null);
    try {
      const resp = await callRpcAwaitingRelay(
        Request.create({ deleteIncidents: { ids: [id], all: false, source } })
      );
      if (resp?.error) {
        setError(resp.error.message);
      }
      await refreshAll();
    } finally {
      setIsLoading(false);
    }
  };

  const deleteAll = async () => {
    setIsLoading(true);
    setError(null);
    setConfirmingDeleteAll(false);
    try {
      const resp = await callRpcAwaitingRelay(
        Request.create({ deleteIncidents: { ids: [], all: true, source } })
      );
      if (resp?.error) {
        setError(resp.error.message);
      }
      await refreshAll();
    } finally {
      setIsLoading(false);
    }
  };

  if (!zmkApp) return null;

  if (!subsystem) {
    return (
      <section className="card">
        <div className="warning-message">
          <p>
            ⚠️ Subsystem "{SUBSYSTEM_IDENTIFIER}" not found. Make sure your
            firmware includes the watchdog module.
          </p>
        </div>
      </section>
    );
  }

  // A fixed small range -- there is no RPC to discover how many peripherals
  // are connected (DESIGN.md SS7), so this just offers "Central" plus a
  // handful of peripheral slots. Selecting a peripheral re-fetches status
  // and incidents relayed from that half's own local store.
  const sourceOptions = [0, 1, 2, 3];

  return (
    <>
      <section className="card">
        <h2>Status</h2>
        <div className="source-selector">
          <label htmlFor="watchdog-source-select">
            <strong>Source:</strong>
          </label>
          <select
            id="watchdog-source-select"
            value={source}
            disabled={isLoading}
            onChange={(e) => setSource(Number(e.target.value))}
          >
            {sourceOptions.map((s) => (
              <option key={s} value={s}>
                {sourceLabel(s)}
              </option>
            ))}
          </select>
        </div>
        {source !== 0 && (
          <div className="warning-message">
            <p>
              ℹ️ Peripheral incidents are relayed over the split link and may
              take a few seconds, or time out if the peripheral is disconnected
              (see README).
            </p>
          </div>
        )}
        {error && (
          <div className="error-message">
            <p>🚨 {error}</p>
          </div>
        )}
        {status ? (
          <div className="status-grid">
            <div>
              <strong>Capacity:</strong> {status.capacity}
            </div>
            <div>
              <strong>Stored:</strong> {status.stored}
            </div>
            <div>
              <strong>Dropped since boot:</strong> {status.droppedSinceBoot}
            </div>
          </div>
        ) : (
          <p>{isLoading ? "⏳ Loading..." : "No status loaded yet."}</p>
        )}
        {status?.recordingStopped && (
          <div className="warning-message">
            <p>
              ⚠️ Log full — recording paused. Delete incidents below to resume
              recording.
            </p>
          </div>
        )}
        <button
          className="btn btn-secondary"
          disabled={isLoading}
          onClick={refreshAll}
        >
          {isLoading ? "⏳ Refreshing..." : "🔄 Refresh"}
        </button>
      </section>

      <section className="card">
        <h2>Incidents ({incidents.length})</h2>
        {incidents.length === 0 ? (
          <p>No incidents recorded.</p>
        ) : (
          <div className="table-scroll">
            <table className="incidents-table">
              <thead>
                <tr>
                  <th>ID</th>
                  <th>Source</th>
                  <th>Type</th>
                  <th>Boot / Uptime</th>
                  <th>Detail</th>
                  <th></th>
                </tr>
              </thead>
              <tbody>
                {incidents.map((incident) => (
                  <tr key={incident.id}>
                    <td>{incident.id}</td>
                    <td>{sourceLabel(incident.source)}</td>
                    <td>
                      <span
                        className={`badge badge-${incidentTypeLabel(
                          incident.type
                        )
                          .toLowerCase()
                          .replace(/\s+/g, "-")}`}
                      >
                        {incidentTypeLabel(incident.type)}
                      </span>
                    </td>
                    <td>
                      #{incident.bootOrdinal} @ {incident.uptimeS}s
                    </td>
                    <td className="detail-cell">
                      {incidentDetailSummary(incident)}
                    </td>
                    <td>
                      <button
                        className="btn btn-danger"
                        disabled={isLoading}
                        onClick={() => deleteOne(incident.id)}
                      >
                        🗑️ Delete
                      </button>
                    </td>
                  </tr>
                ))}
              </tbody>
            </table>
          </div>
        )}

        {incidents.length > 0 && (
          <div className="delete-all-row">
            {confirmingDeleteAll ? (
              <>
                <span>Delete all {incidents.length} incidents?</span>
                <button
                  className="btn btn-danger"
                  disabled={isLoading}
                  onClick={deleteAll}
                >
                  Confirm delete all
                </button>
                <button
                  className="btn btn-secondary"
                  disabled={isLoading}
                  onClick={() => setConfirmingDeleteAll(false)}
                >
                  Cancel
                </button>
              </>
            ) : (
              <button
                className="btn btn-danger"
                disabled={isLoading}
                onClick={() => setConfirmingDeleteAll(true)}
              >
                🗑️ Delete All
              </button>
            )}
          </div>
        )}
      </section>
    </>
  );
}

// Re-exported for tests that want to reference the configured page size.
export const WATCHDOG_RPC_PAGE_SIZE = PAGE_SIZE;
