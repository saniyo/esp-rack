import React, { createContext, useState, useRef, useCallback, useEffect, ReactNode } from 'react';
import { useSnackbar } from 'notistack';

interface AudioContextType {
    isConnected: boolean;
    isStreaming: boolean;
    monitorEnabled: boolean;
    audioLevel: number; // 0.0 to 1.0 (approximated RMS)
    connect: () => void;
    disconnect: () => void;
    startMic: () => void;
    stopMic: () => void;
    toggleMonitor: (enabled: boolean) => void;
    playChunk: (data: Int16Array) => void;
    addDataListener: (listener: (data: Int16Array) => void) => void;
    removeDataListener: (listener: (data: Int16Array) => void) => void;
}

export const AudioContext = createContext<AudioContextType | null>(null);

const WS_URL = "ws://" + window.location.host + "/ws/audio";

export const AudioProvider: React.FC<{ children: ReactNode }> = ({ children }) => {
    const { enqueueSnackbar } = useSnackbar();

    // State
    const [isConnected, setIsConnected] = useState(false);
    const [isStreaming, setIsStreaming] = useState(false);
    const [monitorEnabled, setMonitorEnabled] = useState(false);
    const [audioLevel, setAudioLevel] = useState(0);

    // Refs
    const ws = useRef<WebSocket | null>(null);
    const audioCtx = useRef<AudioContext | null>(null);
    const nextTime = useRef<number>(0);
    const dataListeners = useRef<((data: Int16Array) => void)[]>([]);

    // Initialize Audio Context (Browser Audio API)
    const initAudio = useCallback(() => {
        if (!audioCtx.current) {
            audioCtx.current = new (window.AudioContext || (window as any).webkitAudioContext)({ sampleRate: 16000 });
            console.log("AudioContext initialized", audioCtx.current.state);
        }
        if (audioCtx.current.state === 'suspended') {
            audioCtx.current.resume();
            console.log("AudioContext resumed");
        }
    }, []);

    const playChunk = useCallback((int16Data: Int16Array) => {
        // Notify listeners (visualizers)
        dataListeners.current.forEach(listener => listener(int16Data));

        // Calculate simplified RMS for level meter
        let sum = 0;
        for (let i = 0; i < int16Data.length; i += 10) { // Subsample for performance
            const val = int16Data[i] / 32768.0;
            sum += val * val;
        }
        const rms = Math.sqrt(sum / (int16Data.length / 10));
        setAudioLevel(Math.min(1.0, rms * 5)); // Amplify a bit for visual effect

        // REMOVED: Direct playback via audioCtx to avoid conflict with PcmWorkletPlayer.
        // The data is already forwarded via dataListeners.
    }, []);

    const connect = useCallback(() => {
        console.log("Connecting to WebSocket:", WS_URL);
        if (ws.current?.readyState === WebSocket.OPEN) {
            console.log("WebSocket already open");
            return;
        }

        try {
            ws.current = new WebSocket(WS_URL);
            ws.current.binaryType = 'arraybuffer';

            ws.current.onopen = () => {
                console.log("WebSocket Connected");
                setIsConnected(true);
                // enqueueSnackbar('Audio Connected', { variant: 'success' });
            };

            ws.current.onmessage = (event) => {
                if (event.data instanceof ArrayBuffer) {
                    const int16 = new Int16Array(event.data);
                    // Audio chunks arrive every ~20 ms while streaming — logging
                    // each one retains every ArrayBuffer in DevTools for the
                    // session; drop the log entirely.
                    playChunk(int16);
                }
            };

            ws.current.onclose = () => {
                console.log("WebSocket Closed");
                setIsConnected(false);
                setIsStreaming(false);
            };

            ws.current.onerror = (e) => {
                console.error("Audio WS Error", e);
                // enqueueSnackbar('Audio WebSocket Error', { variant: 'error' });
            };

        } catch (e) {
            console.error(e);
        }
    }, [playChunk]); // Removed enqueueSnackbar dependency to avoid potential infinite loops if called in render

    const disconnect = useCallback(() => {
        console.log("Disconnecting WebSocket");
        if (ws.current) {
            ws.current.close();
            ws.current = null;
        }
        setIsConnected(false);
        setIsStreaming(false);
    }, []);

    const startMic = useCallback(() => {
        console.log("startMic called");
        if (!isConnected) {
            console.log("Not connected, connecting first...");
            connect();
        }

        // Wait for connection if not ready? For now, assume user presses button again or we handle queued msg
        // Better: send message in onOpen if pending. 
        // For simplicity:
        if (ws.current && ws.current.readyState === WebSocket.OPEN) {
            console.log("Sending mic_start command");
            ws.current.send(JSON.stringify({ cmd: "mic_start" }));
            setIsStreaming(true);
        } else {
            console.log("WebSocket not ready, retrying in 1s...");
            // Reconnect and try again?
            connect();
            // Just set a timeout to try sending? 
            // Ideally we'd have a queue. Let's just rely on UI state for now.
            setTimeout(() => {
                if (ws.current && ws.current.readyState === WebSocket.OPEN) {
                    console.log("Retrying mic_start command");
                    ws.current.send(JSON.stringify({ cmd: "mic_start" }));
                    setIsStreaming(true);
                } else {
                    console.log("Retry failed, WebSocket still not open");
                }
            }, 1000);
        }
    }, [connect, isConnected]);

    const stopMic = useCallback(() => {
        console.log("stopMic called");
        if (ws.current && ws.current.readyState === WebSocket.OPEN) {
            console.log("Sending mic_stop command");
            ws.current.send(JSON.stringify({ cmd: "mic_stop" }));
        }
        setIsStreaming(false);
        setAudioLevel(0);
    }, []);

    const toggleMonitor = useCallback((enabled: boolean) => {
        console.log("toggleMonitor", enabled);
        setMonitorEnabled(enabled);
        if (ws.current && ws.current.readyState === WebSocket.OPEN) {
            ws.current.send(JSON.stringify({ cmd: enabled ? "monitor_on" : "monitor_off" }));
        }
    }, []);

    const addDataListener = useCallback((listener: (data: Int16Array) => void) => {
        dataListeners.current.push(listener);
    }, []);

    const removeDataListener = useCallback((listener: (data: Int16Array) => void) => {
        dataListeners.current = dataListeners.current.filter(l => l !== listener);
    }, []);

    // Auto-disconnect on unmount? Maybe not, allow background playing?
    // Let's keep it alive for now as per user request to see indicator everywhere.

    return (
        <AudioContext.Provider value={{
            isConnected,
            isStreaming,
            monitorEnabled,
            audioLevel,
            connect,
            disconnect,
            startMic,
            stopMic,
            toggleMonitor,
            playChunk,
            addDataListener,
            removeDataListener
        }}>
            {children}
        </AudioContext.Provider>
    );
};
