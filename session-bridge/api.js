// SPDX-License-Identifier: GPL-2.0-only
// SPDX-FileCopyrightText: 2026 Zak Noble-Clarke
export const BUS_NAME = 'org.mx.GuestSession';
export const OBJECT_PATH = '/org/mx/GuestSession';
export const INTERFACE = 'org.mx.GuestSession1';
export const XML = `<node>
  <interface name="${INTERFACE}">
    <method name="ListWindows"><arg name="inventory" type="s" direction="out"/></method>
    <method name="ListApplications"><arg name="catalogue" type="s" direction="out"/></method>
    <method name="Activate"><arg name="session" type="s" direction="in"/><arg name="generation" type="s" direction="in"/><arg name="window" type="s" direction="in"/><arg name="result" type="s" direction="out"/></method>
    <method name="Close"><arg name="session" type="s" direction="in"/><arg name="generation" type="s" direction="in"/><arg name="window" type="s" direction="in"/><arg name="result" type="s" direction="out"/></method>
    <method name="Launch"><arg name="session" type="s" direction="in"/><arg name="generation" type="s" direction="in"/><arg name="desktopId" type="s" direction="in"/><arg name="result" type="s" direction="out"/></method>
    <signal name="WindowInventoryChanged"><arg name="session" type="s"/><arg name="generation" type="s"/></signal>
    <signal name="ApplicationCatalogueChanged"><arg name="session" type="s"/><arg name="generation" type="s"/></signal>
  </interface>
</node>`;

export class API {
    constructor(inventory, applications) {
        this.inventory = inventory;
        this.applications = applications;
    }

    ListWindows() {
        return JSON.stringify(this.inventory.refresh());
    }

    ListApplications() {
        return JSON.stringify(this.applications.refresh());
    }

    Activate(session, generation, window) {
        return JSON.stringify(this.inventory.control('activate', session, generation, window));
    }

    Close(session, generation, window) {
        return JSON.stringify(this.inventory.control('close', session, generation, window));
    }

    Launch(session, generation, desktopId) {
        return JSON.stringify(this.applications.launch(session, generation, desktopId));
    }
}
