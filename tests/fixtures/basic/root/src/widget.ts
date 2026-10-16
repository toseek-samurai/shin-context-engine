export interface WidgetRequest {
  capabilityId: string;
  method: "trigger";
}

export const triggerWidget = async (request: WidgetRequest): Promise<string> => {
  const assistantDefault = request.capabilityId ?? "default";
  return `${assistantDefault}:${request.method}`;
};
