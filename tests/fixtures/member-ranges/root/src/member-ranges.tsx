import local from "./tsx-target";

@decorator()
export class TsxRange {
  @decorator()
  render(
    value: string,
  ): JSX.Element {
    return <section>{value}</section>;
  }

  constructor(
    seed: string,
  ) {
    use(seed);
  }
}

export const TsxArrow = (
  value: string,
): JSX.Element => <main>{value}</main>;

render({ value: "call" });
