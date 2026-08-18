import { initializeApp, getApps, getApp } from "firebase/app";
import { getAnalytics, isSupported } from "firebase/analytics";
import { getDatabase, ref, set, push, onValue, serverTimestamp } from "firebase/database";

// Firebase configuration provided for neurospeak2
export const firebaseConfig = {
  apiKey: "AIzaSyCIWR_XGD-UGltZge3hIoDmqtGavsXLFcs",
  authDomain: "neurospeak2.firebaseapp.com",
  databaseURL: "https://neurospeak2-default-rtdb.firebaseio.com",
  projectId: "neurospeak2",
  storageBucket: "neurospeak2.firebasestorage.app",
  messagingSenderId: "126128881547",
  appId: "1:126128881547:web:c8c324d971db041d34f98e",
  measurementId: "G-HLNZSLXTL6"
};

// Initialize Firebase App singleton
export const app = !getApps().length ? initializeApp(firebaseConfig) : getApp();

// Initialize Realtime Database
export const rtdb = getDatabase(app);

// Safe client-side analytics initialization
export const initAnalytics = async () => {
  if (typeof window !== "undefined") {
    const supported = await isSupported();
    if (supported) {
      return getAnalytics(app);
    }
  }
  return null;
};

// Database references
export const RTDB_PATHS = {
  SYSTEM_STATUS: "neurospeak/realtime/system_activated",
  BLINK_COUNT: "neurospeak/realtime/blink_count",
  SELECTED_OUTPUT: "neurospeak/realtime/selected_output",
  CURRENT_NAVIGATION: "neurospeak/realtime/current_navigation",
  LIVE_STATE: "neurospeak/realtime/live_state",
  EVENT_LOGS: "neurospeak/history/events"
};

export interface SystemStatusPayload {
  activated: boolean;
  status: "ACTIVE" | "INACTIVE";
  timestamp: number | object;
}

export interface BlinkDetectionPayload {
  blinkCount: number;
  sequenceCount?: number;
  phase: "DETECTED" | "SEQUENCE_COMPLETED" | "RESET";
  timestamp: number | object;
}

export interface SelectedOutputPayload {
  optionId: string;
  label: string;
  index: number;
  timestamp: number | object;
}

export interface CurrentNavigationPayload {
  index: number;
  optionId: string;
  label: string;
  timestamp: number | object;
}

export interface LiveStatePayload {
  systemActivated: boolean;
  lastBlinkCount: number;
  selectedOutput: string | null;
  selectedLabel: string | null;
  currentNavIndex: number;
  currentNavLabel: string | null;
  lastUpdated: number | object;
  isConnected: boolean;
}

// -------------------------------------------------------------
// Realtime Database Update Helpers
// -------------------------------------------------------------

/**
 * Update system activated state (true/false) in Firebase Realtime Database
 */
export async function syncSystemState(activated: boolean) {
  try {
    const payload: SystemStatusPayload = {
      activated,
      status: activated ? "ACTIVE" : "INACTIVE",
      timestamp: Date.now()
    };
    await set(ref(rtdb, RTDB_PATHS.SYSTEM_STATUS), payload);

    // Also update composite live state
    await set(ref(rtdb, `${RTDB_PATHS.LIVE_STATE}/systemActivated`), activated);
    await set(ref(rtdb, `${RTDB_PATHS.LIVE_STATE}/lastUpdated`), Date.now());

    // Push to event log
    await push(ref(rtdb, RTDB_PATHS.EVENT_LOGS), {
      type: "SYSTEM_ACTIVATION",
      activated,
      timestamp: Date.now()
    });
  } catch (error) {
    console.error("Firebase syncSystemState error:", error);
  }
}

/**
 * Update realtime blink detection & blink count in Firebase Realtime Database
 */
export async function syncBlinkDetection(count: number, phase: "DETECTED" | "SEQUENCE_COMPLETED" | "RESET" = "DETECTED") {
  try {
    const payload: BlinkDetectionPayload = {
      blinkCount: count,
      phase,
      timestamp: Date.now()
    };
    await set(ref(rtdb, RTDB_PATHS.BLINK_COUNT), payload);

    // Also update composite live state
    await set(ref(rtdb, `${RTDB_PATHS.LIVE_STATE}/lastBlinkCount`), count);
    await set(ref(rtdb, `${RTDB_PATHS.LIVE_STATE}/lastUpdated`), Date.now());

    // Push to event log
    await push(ref(rtdb, RTDB_PATHS.EVENT_LOGS), {
      type: "BLINK_EVENT",
      count,
      phase,
      timestamp: Date.now()
    });
  } catch (error) {
    console.error("Firebase syncBlinkDetection error:", error);
  }
}

/**
 * Update selected output item in Firebase Realtime Database
 */
export async function syncSelectedOutput(optionId: string, label: string, index: number) {
  try {
    const payload: SelectedOutputPayload = {
      optionId,
      label,
      index,
      timestamp: Date.now()
    };
    await set(ref(rtdb, RTDB_PATHS.SELECTED_OUTPUT), payload);

    // Also update composite live state
    await set(ref(rtdb, `${RTDB_PATHS.LIVE_STATE}/selectedOutput`), optionId);
    await set(ref(rtdb, `${RTDB_PATHS.LIVE_STATE}/selectedLabel`), label);
    await set(ref(rtdb, `${RTDB_PATHS.LIVE_STATE}/lastUpdated`), Date.now());

    // Push to event log
    await push(ref(rtdb, RTDB_PATHS.EVENT_LOGS), {
      type: "OUTPUT_SELECTED",
      optionId,
      label,
      index,
      timestamp: Date.now()
    });
  } catch (error) {
    console.error("Firebase syncSelectedOutput error:", error);
  }
}

/**
 * Update current menu navigation highlight in Firebase Realtime Database
 */
export async function syncCurrentNavigation(index: number, optionId: string, label: string) {
  try {
    const payload: CurrentNavigationPayload = {
      index,
      optionId,
      label,
      timestamp: Date.now()
    };
    await set(ref(rtdb, RTDB_PATHS.CURRENT_NAVIGATION), payload);

    // Also update composite live state
    await set(ref(rtdb, `${RTDB_PATHS.LIVE_STATE}/currentNavIndex`), index);
    await set(ref(rtdb, `${RTDB_PATHS.LIVE_STATE}/currentNavLabel`), label);
    await set(ref(rtdb, `${RTDB_PATHS.LIVE_STATE}/lastUpdated`), Date.now());
  } catch (error) {
    console.error("Firebase syncCurrentNavigation error:", error);
  }
}

/**
 * Update overall connection status in Firebase
 */
export async function syncConnectionState(isConnected: boolean) {
  try {
    await set(ref(rtdb, `${RTDB_PATHS.LIVE_STATE}/isConnected`), isConnected);
    await set(ref(rtdb, `${RTDB_PATHS.LIVE_STATE}/lastUpdated`), Date.now());
  } catch (error) {
    console.error("Firebase syncConnectionState error:", error);
  }
}
