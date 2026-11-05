import './App.css'
import { Activity, HeartPulse, Wifi, WifiOff, AlertCircle, Droplets, Thermometer } from 'lucide-react';
import { Card, CardContent, CardDescription, CardHeader, CardTitle } from './components/ui/card';
import useVitalSigns from './hooks/use-vital-signs'
import { Tabs, TabsContent } from './components/ui/tabs';
import { Badge } from './components/ui/badge';
import WaveformCanvas from './components/waveform-canvas';

function App() {
  const { data, connected, socket } = useVitalSigns();

  return (
    <div className='bg-muted w-screen min-h-screen h-auto flex flex-col gap-4 justify-start items-center py-6 px-4 md:px-10 overflow-x-hidden'>
      <div className='w-full flex flex-col gap-2 justify-center items-center max-w-7xl mx-auto'>
        <div className='flex gap-2 justify-start items-center'>
          {/*<HeartPlus className='shrink-0 text-cyan-600 size-8' />*/}
          <p className='text-3xl font-semibold'>Monitor de Sinais Vitais</p>
        </div>
        {connected ?
          <Badge className='py-3.5' variant='default'><Wifi className="mr-2" /> Conectado</Badge> :
          <Badge className='py-3.5' variant='destructive'><WifiOff className="mr-2" /> Desconectado</Badge>}
      </div>

      <Tabs defaultValue='dashboard' className='w-full max-w-7xl mx-auto items-start'>
        <TabsContent value='dashboard' className='w-full'>
          <div className='w-full grid grid-cols-1 gap-6 items-center'>

            <div className='w-full grid grid-cols-1 lg:grid-cols-2 gap-4 mt-4'>
              <WaveformCanvas
                socket={socket}
                type="ecg"
                size={250}
                color="#4ade80"
                minSpan={5}
                label="ECG Filtrado"
                unit="mV"
              />
              <WaveformCanvas
                socket={socket}
                type="ppg"
                size={200}
                color="#38bdf8"
                minSpan={100}
                label="PPG (Oximetria - IR)"
                unit="units"
              />
            </div>

            <div className='w-full grid grid-cols-1 md:grid-cols-2 xl:grid-cols-4 gap-4'>
              <Card>
                <CardHeader>
                  <CardTitle className='flex gap-1 items-center justify-between'>
                    <p className='text-2xl'>ECG</p>
                    {data.leadoff ? <Badge className='py-3.5' variant='destructive' ><AlertCircle className="mr-1" /> Eletrodos desconectados</Badge> : <></>}
                  </CardTitle>
                  <CardDescription>Eletrocardiograma</CardDescription>
                </CardHeader>
                <CardContent className='flex gap-4 justify-start items-center'>
                  <div className='shrink-0 shadow-lg size-24 rounded-full bg-red-200 flex p-4 items-center justify-center'>
                    <HeartPulse className='text-red-600 size-10' />
                  </div>
                  {!data.leadoff ? <div className='flex flex-col'>
                    <p className='text-2xl text-muted-foreground'>Batimentos</p>
                    <p className='text-3xl'>{data.bpmECG}</p>
                    <p className='text-xl text-muted-foreground'>BPM</p>
                  </div> : <p className='text-base text-red-600'>Leitura inválida</p>}
                </CardContent>
              </Card>

              <Card>
                <CardHeader>
                  <CardTitle className='flex gap-1 items-center justify-between'>
                    <p className='text-2xl'>PPG</p>
                    {!data.fingerPresent ? <Badge className='py-3.5' variant='destructive' ><AlertCircle className="mr-1" /> Dedo ausente</Badge> : <></>}
                  </CardTitle>
                  <CardDescription>Fotopletismografia</CardDescription>
                </CardHeader>
                <CardContent className='flex gap-4 justify-start items-center'>
                  <div className='shrink-0 shadow-lg size-24 rounded-full bg-blue-200 flex p-4 items-center justify-center'>
                    <Activity className='text-blue-600 size-10' />
                  </div>
                  {data.bpmPPGValid ? <div className='flex flex-col'>
                    <p className='text-2xl text-muted-foreground'>Batimentos</p>
                    <p className='text-3xl'>{data.bpmPPG}</p>
                    <p className='text-xl text-muted-foreground'>BPM</p>
                  </div> : <p className='text-base text-red-600'>Leitura inválida</p>}
                </CardContent>
              </Card>

              <Card>
                <CardHeader>
                  <CardTitle className='flex gap-1 items-center justify-between'>
                    <p className='text-2xl'>SpO2</p>
                    {!data.fingerPresent ? <Badge className='py-3.5' variant='destructive' ><AlertCircle className="mr-1" /> Dedo ausente</Badge> : <></>}
                  </CardTitle>
                  <CardDescription>Nível de oxigênio no sangue</CardDescription>
                </CardHeader>
                <CardContent className='flex gap-4 justify-start items-center'>
                  <div className='shrink-0 shadow-lg size-24 rounded-full bg-green-200 flex p-4 items-center justify-center'>
                    <Droplets className='text-green-600 size-10' />
                  </div>
                  {data.spo2Valid ? <div className='flex flex-col'>
                    <p className='text-2xl text-muted-foreground'>Saturação</p>
                    <p className='text-3xl'>{data.spo2} %</p>
                  </div> : <p className='text-base text-red-600'>Leitura inválida</p>}
                </CardContent>
              </Card>

              <Card>
                <CardHeader>
                  <CardTitle className='flex gap-1 items-center justify-between'>
                    <p className='text-2xl'>Temp</p>
                    {!data.tempValid ? <Badge className='py-3.5' variant='destructive' ><AlertCircle className="mr-1" /> Sem sensor</Badge> : <></>}
                  </CardTitle>
                  <CardDescription>Temperatura Corporal</CardDescription>
                </CardHeader>
                <CardContent className='flex gap-4 justify-start items-center'>
                  <div className='shrink-0 shadow-lg size-24 rounded-full bg-amber-200 flex p-4 items-center justify-center'>
                    <Thermometer className='text-amber-600 size-10' />
                  </div>
                  {data.tempValid ? <div className='flex flex-col'>
                    <p className='text-2xl text-muted-foreground'>Celsius</p>
                    <p className='text-3xl'>{data.tempC.toFixed(1)} °C</p>
                  </div> : <p className='text-base text-red-600'>Leitura inválida</p>}
                </CardContent>
              </Card>
            </div>
          </div>
        </TabsContent>
      </Tabs>
    </div>
  )
}

export default App