export const ActorWorkerBridge = (
  hostCallSession: string,
  requestId: string,
  resultValue: unknown,
  affinityToken: string,
  routingKey: string,
  commandEnvelope: unknown,
  validationReport: unknown,
) => ({
  hostCallSession,
  requestId,
  resultValue,
  affinityToken,
  routingKey,
  commandEnvelope,
  validationReport,
});
