package example

typealias AccountId = String

@JvmInline value class AccountKey(val value: String)

data class Account(val id: AccountId)

object AccountMapper {
    fun loadAccount(id: AccountId): Account = Account(id)
}

fun Account.displayName(): String = "Account $id"

val accountFactory = fun(id: AccountId): Account = Account(id)
