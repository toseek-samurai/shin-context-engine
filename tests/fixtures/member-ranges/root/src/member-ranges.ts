import local from "./relative-target";
export { local } from "./re-export-target";
import ambiguous from "./ambiguous";
import escaped from "./escaped\\path";
import packageValue from "package-value";

@decorator()
export class TypeRange {
  @decorator()
  public multilineMember(
    value: string,
    retries: number,
  ): string {
    return value;
  }

  overloaded(value: string): string;
  overloaded(value: string): string {
    return value;
  }

  constructor(
    seed: string,
  ) {
    use(seed);
  }
}

export type RangeAlias = string;

export const multilineArrow = (
  value: string,
): string => {
  return value;
};

run({ value: 1 });
