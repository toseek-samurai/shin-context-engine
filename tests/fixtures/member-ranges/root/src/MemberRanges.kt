class KotlinRange {
    fun multilineMember(
        value: String,
        retries: Int,
    ): String {
        return "$value:$retries"
    }

    fun callOnly() {
        multilineMember("call", 1)
    }
}
