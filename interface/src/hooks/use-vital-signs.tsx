import { useState, useEffect } from 'react'
import type { Data, DataRaw } from '@/types/data';

export default function useVitalSigns() {

  const [data, setData] = useState<Data>({
    leadoff: true,
    bpmECG: 0,
    fingerPresent: false,
    bpmPPGValid: false,
    bpmPPG: 0,
    spo2Valid: false,
    spo2: 0
  });
  const [connected, setConnected] = useState(false);

  function dataAdapter(data: DataRaw): Data {
    return {
      leadoff: data.leadoff == 0 ? false : true,
      bpmECG: data.bpmECG ?? 0.0,
      fingerPresent: data.fingerPresent == 0 ? false : true,
      bpmPPGValid: data.bpmPPGValid == 0 ? false : true,
      bpmPPG: data.bpmPPG ?? 0,
      spo2Valid: data.spo2Valid == 0 ? false : true,
      spo2: data.spo2 ?? 0
    }
  }

  useEffect(() => {
    const ip = window.location.hostname ?? '192.168.4.1';
    const ws = new WebSocket(`ws://${ip}/ws`);

    ws.onopen = () => setConnected(true);
    ws.onclose = () => setConnected(false);

    ws.onmessage = (event) => {
      const parsed = JSON.parse(event.data);
      setData(dataAdapter(parsed));
    }

    return () => ws.close();
  }, []);

  return { data, connected };
}