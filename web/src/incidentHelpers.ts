import { Incident, IncidentType } from "./proto/cormoran/watchdog/watchdog";

/** Zephyr's k_fatal_error_reason (zephyr/include/zephyr/fatal_types.h). */
const K_ERR_REASON_NAMES: Record<number, string> = {
  0: "CPU_EXCEPTION",
  1: "SPURIOUS_IRQ",
  2: "STACK_CHK_FAIL",
  3: "KERNEL_OOPS",
  4: "KERNEL_PANIC",
};

export function fatalReasonName(reason: number): string {
  return K_ERR_REASON_NAMES[reason] ?? `UNKNOWN(${reason})`;
}

export function toHex(value: number): string {
  return "0x" + (value >>> 0).toString(16).padStart(8, "0");
}

/**
 * Zephyr hwinfo reset-cause bits (zephyr/include/zephyr/drivers/hwinfo.h).
 * Only a subset is "of interest" per src/watchdog_reset_cause.c
 * (WATCHDOG_RESET_CAUSE_OF_INTEREST = RESET_WATCHDOG | RESET_CPU_LOCKUP |
 * RESET_BROWNOUT), but decoding the full set makes the raw cause_bits more
 * readable if a board ever surfaces others alongside them.
 */
const RESET_CAUSE_BITS: Array<[bit: number, name: string]> = [
  [0, "PIN"],
  [1, "SOFTWARE"],
  [2, "BROWNOUT"],
  [3, "POR"],
  [4, "WATCHDOG"],
  [5, "DEBUG"],
  [6, "SECURITY"],
  [7, "LOW_POWER_WAKE"],
  [8, "CPU_LOCKUP"],
  [9, "PARITY"],
  [10, "PLL"],
  [11, "CLOCK"],
  [12, "HARDWARE"],
  [13, "USER"],
  [14, "TEMPERATURE"],
];

export function resetCauseLabels(causeBits: number): string[] {
  const labels: string[] = [];
  for (const [bit, name] of RESET_CAUSE_BITS) {
    if ((causeBits & (1 << bit)) !== 0) {
      labels.push(name);
    }
  }
  return labels;
}

export function incidentTypeLabel(type: IncidentType): string {
  switch (type) {
    case IncidentType.FREEZE:
      return "Freeze";
    case IncidentType.FATAL:
      return "Fatal";
    case IncidentType.RESET_CAUSE:
      return "Reset Cause";
    default:
      return "Unknown";
  }
}

export function sourceLabel(source: number): string {
  return source === 0 ? "Central" : `Peripheral ${source - 1}`;
}

/** Short, single-line summary of an incident's detail column. */
export function incidentDetailSummary(incident: Incident): string {
  if (incident.freeze) {
    return `queue=${incident.freeze.queueName || "?"} channel=${incident.freeze.channelId}`;
  }
  if (incident.fatal) {
    return `${fatalReasonName(incident.fatal.reason)} pc=${toHex(
      incident.fatal.pc
    )} lr=${toHex(incident.fatal.lr)} thread=${incident.fatal.threadName || "?"}`;
  }
  if (incident.reset) {
    const labels = resetCauseLabels(incident.reset.causeBits);
    const decoded =
      labels.length > 0 ? labels.join(", ") : "no recognized bits";
    return `${toHex(incident.reset.causeBits)} (${decoded})`;
  }
  return "-";
}
