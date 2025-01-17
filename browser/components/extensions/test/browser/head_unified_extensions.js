/* Any copyright is dedicated to the Public Domain.
   http://creativecommons.org/publicdomain/zero/1.0/ */

"use strict";

/* exported clickUnifiedExtensionsItem,
            closeExtensionsPanel,
            createExtensions,
            getUnifiedExtensionsItem,
            openExtensionsPanel,
            openUnifiedExtensionsContextMenu,
            promiseDisableUnifiedExtensions,
            promiseEnableUnifiedExtensions
*/

const promiseEnableUnifiedExtensions = async () => {
  await SpecialPowers.pushPrefEnv({
    set: [["extensions.unifiedExtensions.enabled", true]],
  });

  return BrowserTestUtils.openNewBrowserWindow();
};

const promiseDisableUnifiedExtensions = async () => {
  await SpecialPowers.pushPrefEnv({
    set: [["extensions.unifiedExtensions.enabled", false]],
  });

  return BrowserTestUtils.openNewBrowserWindow();
};

const getListView = win => {
  const { panel } = win.gUnifiedExtensions;
  ok(panel, "expected panel to be created");
  return panel.querySelector("#unified-extensions-view");
};

const openExtensionsPanel = async win => {
  const { button } = win.gUnifiedExtensions;
  ok(button, "expected button");

  const listView = getListView(win);
  ok(listView, "expected list view");

  const viewShown = BrowserTestUtils.waitForEvent(listView, "ViewShown");
  button.click();
  await viewShown;
};

const closeExtensionsPanel = async win => {
  const { button } = win.gUnifiedExtensions;
  ok(button, "expected button");

  const hidden = BrowserTestUtils.waitForEvent(
    win.document,
    "popuphidden",
    true
  );
  button.click();
  await hidden;
};

const getUnifiedExtensionsItem = (win, extensionId) => {
  const view = getListView(win);

  // First try to find a CUI widget, otherwise a custom element when the
  // extension does not have a browser action.
  return (
    view.querySelector(`toolbaritem[data-extensionid="${extensionId}"]`) ||
    view.querySelector(`unified-extensions-item[extension-id="${extensionId}"]`)
  );
};

const openUnifiedExtensionsContextMenu = async (win, extensionId) => {
  const item = getUnifiedExtensionsItem(win, extensionId);
  ok(item, `expected item for extensionId=${extensionId}`);
  const button = item.querySelector(".unified-extensions-item-open-menu");
  ok(button, "expected 'open menu' button");
  // Make sure the button is visible before clicking on it (below) since the
  // list of extensions can have a scrollbar (when there are many extensions
  // and/or the window is small-ish).
  button.scrollIntoView({ block: "center" });

  const menu = win.document.getElementById("unified-extensions-context-menu");
  ok(menu, "expected menu");

  const shown = BrowserTestUtils.waitForEvent(menu, "popupshown");
  // Use primary button click to open the context menu.
  EventUtils.synthesizeMouseAtCenter(button, {}, win);
  await shown;

  return menu;
};

const clickUnifiedExtensionsItem = async (
  win,
  extensionId,
  forceEnableButton = false
) => {
  // The panel should be closed automatically when we click an extension item.
  await openExtensionsPanel(win);

  const item = getUnifiedExtensionsItem(win, extensionId);
  ok(item, `expected item for ${extensionId}`);

  // The action button should be disabled when users aren't supposed to click
  // on it but it might still be useful to re-enable it for testing purposes.
  if (forceEnableButton) {
    let actionButton = item.querySelector(".unified-extensions-item-action");
    actionButton.disabled = false;
    ok(!actionButton.disabled, "action button was force-enabled");
  }

  // Similar to `openUnifiedExtensionsContextMenu()`, we make sure the item is
  // visible before clicking on it to prevent intermittents.
  item.scrollIntoView({ block: "center" });

  const popupHidden = BrowserTestUtils.waitForEvent(
    win.document,
    "popuphidden",
    true
  );
  EventUtils.synthesizeMouseAtCenter(item, {}, win);
  await popupHidden;
};

let extensionsCreated = 0;
const createExtensions = (
  arrayOfManifestData,
  { useAddonManager = true } = {}
) => {
  return arrayOfManifestData.map(manifestData =>
    ExtensionTestUtils.loadExtension({
      manifest: {
        name: "default-extension-name",
        browser_specific_settings: {
          gecko: { id: `@ext-${extensionsCreated++}` },
        },
        ...manifestData,
      },
      useAddonManager: useAddonManager ? "temporary" : undefined,
    })
  );
};
