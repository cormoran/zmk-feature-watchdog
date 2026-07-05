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

interface Status {
  capacity: number;
  stored: number;
  droppedSinceBoot: number;
  recordingStopped: boolean;
}

export function IncidentsSection() {
  const zmkApp = useContext(ZMKAppContext);
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

  const refreshStatus = useCallback(async () => {
    const resp = await callRpc(Request.create({ getStatus: {} }));
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
  }, [callRpc]);

  const refreshIncidents = useCallback(async () => {
    const collected: Incident[] = [];
    let startIndex = 0;
    // Fetch all pages on connect/refresh -- the store is capped (default 16
    // incidents), so this is at most a handful of round-trips.
    for (let guard = 0; guard < 64; guard++) {
      const resp = await callRpc(
        Request.create({ listIncidents: { startIndex } })
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
  }, [callRpc]);

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
    // Run once when the connected watchdog subsystem becomes available.
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [subsystem?.index, zmkApp?.state.connection]);

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
        if (notification.incidentRecorded?.incident) {
          const incident = notification.incidentRecorded.incident;
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
  }, [zmkApp, subsystem?.index]);

  const deleteOne = async (id: number) => {
    setIsLoading(true);
    setError(null);
    try {
      const resp = await callRpc(
        Request.create({ deleteIncidents: { ids: [id], all: false } })
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
      const resp = await callRpc(
        Request.create({ deleteIncidents: { ids: [], all: true } })
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

  return (
    <>
      <section className="card">
        <h2>Status</h2>
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
