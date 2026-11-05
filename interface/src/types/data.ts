export interface DataRaw {
  leadoff: number,
  bpmECG: number,
  fingerPresent: number,
  bpmPPGValid: number,
  bpmPPG: number,
  spo2Valid: number,
  spo2: number
}

export interface Data {
  leadoff: boolean,
  bpmECG: number,
  fingerPresent: boolean,
  bpmPPGValid: boolean,
  bpmPPG: number,
  spo2Valid: boolean,
  spo2: number
}