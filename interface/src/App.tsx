import './App.css'
import { Activity, HeartPulse, Wifi, WifiOff, AlertCircle, Droplets, HeartPlus } from 'lucide-react';
import { Card, CardContent, CardDescription, CardHeader, CardTitle } from './components/ui/card';
import useVitalSigns from './hooks/use-vital-signs'
import { Tabs, TabsContent } from './components/ui/tabs';
import { Badge } from './components/ui/badge';
import useBreakpoints from './hooks/use-breakpoints';

function App() {

  const { } = useBreakpoints
  const { data, connected } = useVitalSigns();

  return (
    <div className='bg-muted w-screen h-screen flex flex-col gap-4 justify-start items-center py-3 px-10'>
      <div className='w-full flex justify-between items-center'>
        <div className='flex gap-2 justify-start items-center'>
          <HeartPlus className='shrink-0 text-cyan-600 size-8' />
          <p className='text-xl font-semibold'>Monitoramento de sinais vitais</p>
        </div>
        {connected ?
          <Badge className='py-3.5' variant='default'><Wifi /> Conectado</Badge> :
          <Badge className='py-3.5' variant='destructive'><WifiOff /> Desconectado</Badge>}
      </div>

      <Tabs defaultValue='dashboard' className='w-full items-start'>


        {/*<TabsList style={{ height: '52px' }} className="w-full md:w-fit">
          <TabsTrigger value='dashboard' className='px-3 text-base'>
            <SquareActivity className='size-5' />
            Dashboard
          </TabsTrigger>
          <TabsTrigger value='config' className='px-3 text-base'>
            <Settings className='size-5' />
            Configurações
          </TabsTrigger>
        </TabsList>*/}

        <TabsContent value='dashboard' className='w-full'>
          <div className='w-full grid grid-cols-1 gap-4 items-center'>
            <div className='grid grid-cols-1 gap-1 justify-start items-center'>
              {/*<h1 className='text-3xl font-semibold'>Monitoramento de Sinais Vitais</h1>*/}
              <h3 className='text-lg text-muted-foreground'>Seja bem vindo, aqui você pode visualizar as informações de batimento cardíaco e de nível de oxigêncio no sangue.</h3>
            </div>

            <div className='w-full grid grid-cols-1 md:grid-cols-2 lg:grid-cols-3 gap-4'>
              <Card>
                <CardHeader>
                  <CardTitle className='flex gap-1 items-center justify-between'>
                    <p className='text-2xl'>ECG</p>
                    {data.leadoff ? <Badge className='py-3.5' variant='destructive' ><AlertCircle /> Eletrodos desconectados</Badge> : <></>}
                  </CardTitle>
                  <CardDescription>Eletrocardiograma</CardDescription>
                </CardHeader>
                <CardContent className='flex gap-4 justify-start items-center'>
                  <div className='shrink-0 shadow-lg size-24 rounded-full bg-red-200 flex p-4 items-center justify-center'>
                    <HeartPulse className='text-red-600 size-10' />
                  </div>
                  <div className='flex flex-col'>
                    <p className='text-2xl text-muted-foreground'>Batimentos</p>
                    <p className='text-3xl'>{data.bpmECG}</p>
                    <p className='text-xl text-muted-foreground'>BPM</p>
                  </div>
                </CardContent>
              </Card>

              <Card>
                <CardHeader>
                  <CardTitle className='flex gap-1 items-center justify-between'>
                    <p className='text-2xl'>PPG</p>
                    {!data.fingerPresent ? <Badge className='py-3.5' variant='destructive' ><AlertCircle /> Dedo ausente</Badge> : <></>}
                  </CardTitle>
                  <CardDescription>Fotopletismografia</CardDescription>
                </CardHeader>
                <CardContent className='flex gap-4 justify-start items-center'>
                  <div className='shrink-0 shadow-lg size-24 rounded-full bg-blue-200 flex p-4 items-center justify-center'>
                    <Activity className='text-blue-600 size-10' />
                  </div>
                  {data.bpmPPGValid ? <div className='flex flex-col'>
                    <p className='text-2xl text-muted-foreground'>Batimentos</p>
                    <p className='text-3xl'>{data.bpmECG}</p>
                    <p className='text-xl text-muted-foreground'>BPM</p>
                  </div> : <p className='text-base text-red-600'>Leitura inválida</p>}
                </CardContent>
              </Card>

              <Card>
                <CardHeader>
                  <CardTitle className='flex gap-1 items-center justify-between'>
                    <p className='text-2xl'>SP02</p>
                    {!data.fingerPresent ? <Badge className='py-3.5' variant='destructive' ><AlertCircle /> Dedo ausente</Badge> : <></>}
                  </CardTitle>
                  <CardDescription>Nível de oxigênio no sangue</CardDescription>
                </CardHeader>
                <CardContent className='flex gap-4 justify-start items-center'>
                  <div className='shrink-0 shadow-lg size-24 rounded-full bg-green-200 flex p-4 items-center justify-center'>
                    <Droplets className='text-green-600 size-10' />
                  </div>
                  {data.spo2Valid ? <div className='flex flex-col'>
                    <p className='text-2xl text-muted-foreground'>Saturação de oxigenio</p>
                    <p className='text-3xl'>{data.spo2} %</p>
                    <p className='text-xl text-muted-foreground'></p>
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
