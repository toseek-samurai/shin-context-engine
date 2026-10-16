import { PriceService } from "../src/PriceService";

describe("price contract", () => {
  it("observes a changed member", () => {
    expect(new PriceService().changedMember(1)).toBe(2);
  });
});
