package example

import org.junit.jupiter.api.Test

class AccountServiceTest {
    @Test fun `loads an account`() {
        check(AccountMapper.loadAccount("42").id == "42")
    }
}
