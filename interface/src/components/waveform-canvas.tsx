import { useEffect, useRef } from 'react';
import { Card, CardContent, CardHeader, CardTitle } from '@/components/ui/card';

interface WaveformCanvasProps {
  socket: WebSocket | null;
  type: 'ecg' | 'ppg';
  size: number;
  color: string;
  minSpan: number;
  label: string;
  unit: string;
}

function WaveformCanvas({ socket, type, size, color, minSpan, label, unit }: WaveformCanvasProps) {
  const canvasRef = useRef<HTMLCanvasElement>(null);
  const scaleRef = useRef<HTMLSpanElement>(null);

  const bufferRef = useRef(new Float32Array(size));
  const headRef = useRef(0);
  const countRef = useRef(0);

  useEffect(() => {
    if (!socket) return;

    const handleMessage = (event: MessageEvent) => {
      try {
        const data = JSON.parse(event.data);
        if (data.w === 1) {
          const waves = type === 'ecg' ? data.ecg : data.ir;
          if (waves && waves.length > 0) {
            const buf = bufferRef.current;
            for (let i = 0; i < waves.length; i++) {
              buf[headRef.current] = waves[i];
              headRef.current = (headRef.current + 1) % size;
              if (countRef.current < size) countRef.current++;
            }
          }
        }
      } catch (e) { }
    };

    socket.addEventListener('message', handleMessage);
    return () => socket.removeEventListener('message', handleMessage);
  }, [socket, type, size]);

  useEffect(() => {
    let animationFrameId: number;

    const render = () => {
      const canvas = canvasRef.current;
      const ctx = canvas?.getContext('2d');
      if (!canvas || !ctx) return;

      const dpr = window.devicePixelRatio || 1;
      const rect = canvas.getBoundingClientRect();
      if (canvas.width !== Math.round(rect.width * dpr) || canvas.height !== Math.round(rect.height * dpr)) {
        canvas.width = Math.max(1, Math.round(rect.width * dpr));
        canvas.height = Math.max(1, Math.round(rect.height * dpr));
        ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
      }

      const w = rect.width;
      const h = rect.height;
      const count = countRef.current;
      const head = headRef.current;
      const buf = bufferRef.current;

      ctx.clearRect(0, 0, w, h);

      // Linha central
      ctx.strokeStyle = '#1e2a44';
      ctx.lineWidth = 1;
      ctx.beginPath();
      ctx.moveTo(0, h / 2);
      ctx.lineTo(w, h / 2);
      ctx.stroke();

      if (count < 2) {
        ctx.fillStyle = '#64748b';
        ctx.font = '14px Arial';
        ctx.textAlign = 'center';
        ctx.fillText('Aguardando dados...', w / 2, h / 2 + 5);
        animationFrameId = requestAnimationFrame(render);
        return;
      }

      // Cálculo de Autoescala dinâmica (Dynamic Auto-scaling)
      let mn = Infinity, mx = -Infinity;
      const start = (head - count + size) % size;
      for (let i = 0; i < count; i++) {
        const v = buf[(start + i) % size];
        if (v < mn) mn = v;
        if (v > mx) mx = v;
      }

      const mid = (mn + mx) / 2;
      const span = Math.max(mx - mn, minSpan) * 1.2;
      const lo = mid - span / 2;
      const range = span || 1;
      const pxPerSample = w / (size - 1);

      // Plotagem da Linha
      ctx.strokeStyle = color;
      ctx.lineWidth = 1.8;
      ctx.lineJoin = 'round';
      ctx.beginPath();
      for (let i = 0; i < count; i++) {
        const v = buf[(start + i) % size];
        const x = i * pxPerSample;
        const y = h - ((v - lo) / range) * (h - 20) - 10;
        if (i === 0) ctx.moveTo(x, y);
        else ctx.lineTo(x, y);
      }
      ctx.stroke();

      if (scaleRef.current) {
        scaleRef.current.textContent = `${range.toFixed(1)} ${unit} p/p`;
      }

      animationFrameId = requestAnimationFrame(render);
    };

    animationFrameId = requestAnimationFrame(render);
    return () => cancelAnimationFrame(animationFrameId);
  }, [color, minSpan, unit, size]);

  return (
    <Card>
      <CardHeader>
        <CardTitle className='flex gap-1 items-center justify-between'>
          <p className='text-2xl'>{label}</p>
          <span ref={scaleRef} className="text-gray-400 text-xs font-mono">--</span>
        </CardTitle>
      </CardHeader>
      <CardContent>
        <canvas
          ref={canvasRef}
          className="w-full h-45 bg-black rounded-lg block"
        />
      </CardContent>
    </Card>
  );
}

export default WaveformCanvas