<App>
  <Include
    src="./queries.rsx"
  />
  <Include src={"./src/View.rsx"} />
  <Include src="./Missing.rsx" />
  <Include src="../../../../outside.rsx" />
  <Include src="./not-indexed.txt" />
  <Include src={"./dynamic.rsx" + suffix} />
  <!-- <Include src="./comment.rsx" /> -->
  {/* <Include src="./js-comment.rsx" /> */}
</App>
