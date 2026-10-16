const response = await RetoolLoadWidget.trigger();
await RetoolModal.show();
return { response, form: RetoolForm.data };
