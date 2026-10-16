export class PriceService {
  changedMember(value: number): number {
    return value + 1;
  }

  stableMember(value: number): number {
    return value * 3;
  }
}
