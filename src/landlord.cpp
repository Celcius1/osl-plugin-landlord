/**
 * ============================================================================
 * SOFTWARE: OSL: Sovereign Accounting Suite
 * AUTHOR & COPYRIGHT: Cel-Tech-Serv Pty Ltd
 * MODULE: Landlord Plugin (Identity & Hierarchical Entity Manager)
 * DESCRIPTION: 
 * Full Sovereign Microkernel implementation. 
 * Completely decoupled from the Core Engine. Injects all necessary 
 * routing, Javascript, Identity UI, and filtering logic directly 
 * via the window.pluginHooks array.
 * ============================================================================
 */

#include "../../../interface/osl_plugin.hpp"
#include <vector>
#include <string>
#include <iostream>
#include <pqxx/pqxx> 
#include <cstdlib>
#include <httplib.h>          
#include <nlohmann/json.hpp>  
#include <thread>
#include <chrono>

using namespace osl;
using json = nlohmann::json;

/**
 * --- SOVEREIGN BOOTSTRAP LOGIC (THREADED) ---
 */
void __attribute__((constructor)) sovereign_identity_init() {
    fprintf(stderr, "[Landlord Cel-Tech-Serv] CRITICAL: Commencing Native Identity Bootstrap (Background Thread)...\n");

    const char* admin_user = std::getenv("LLDAP_ADMIN_USER");
    const char* admin_pass = std::getenv("LLDAP_ADMIN_PASS");

    if (!admin_pass) {
        fprintf(stderr, "[Landlord Cel-Tech-Serv] FATAL: LLDAP_ADMIN_PASS missing. Aborting bootstrap.\n");
        return;
    }

    std::string user = admin_user ? admin_user : "admin";
    std::string pass = admin_pass;

    std::thread([user, pass]() {
        std::this_thread::sleep_for(std::chrono::seconds(10));

        httplib::Client lldap("http://osl-identity:17170");
        lldap.set_connection_timeout(5, 0); 
        lldap.set_read_timeout(5, 0);       

        json auth_payload = {{"username", user}, {"password", pass}};
        auto auth_res = lldap.Post("/auth/simple/login", auth_payload.dump(), "application/json");
        
        if (!auth_res || auth_res->status != 200) {
            fprintf(stderr, "[Landlord Cel-Tech-Serv] FATAL: Auth Failed. LLDAP not ready or bad credentials.\n");
            return;
        }

        std::string token = json::parse(auth_res->body)["token"];
        httplib::Headers headers = {{"Authorization", "Bearer " + token}};
        fprintf(stderr, "[Landlord Cel-Tech-Serv] Native JWT Acquired. Verifying Compliance...\n");

        json query_payload = {{"query", "query { groups { id displayName } }"}};
        auto group_res = lldap.Post("/api/graphql", headers, query_payload.dump(), "application/json");
        
        if (!group_res || group_res->status != 200) {
            fprintf(stderr, "[Landlord Cel-Tech-Serv] FATAL: Failed to query existing groups.\n");
            return;
        }

        auto existing_data = json::parse(group_res->body);
        std::vector<std::string> required_groups = {"admins", "accountants", "clients"};
        int admins_group_id = -1;

        for (const auto& g : required_groups) {
            bool found = false;
            if (existing_data.contains("data") && existing_data["data"].contains("groups")) {
                for (const auto& existing : existing_data["data"]["groups"]) {
                    if (existing["displayName"] == g) {
                        found = true;
                        if (g == "admins") admins_group_id = existing["id"].get<int>();
                        fprintf(stderr, "[Landlord Cel-Tech-Serv] Compliant Group found: '%s'\n", g.c_str());
                        break;
                    }
                }
            }

            if (!found) {
                fprintf(stderr, "[Landlord Cel-Tech-Serv] Group '%s' missing. Provisioning natively...\n", g.c_str());
                json create_payload = {
                    {"query", "mutation CreateGroup($name: String!) { createGroup(name: $name) { id } }"},
                    {"variables", {{"name", g}}}
                };
                auto create_res = lldap.Post("/api/graphql", headers, create_payload.dump(), "application/json");
                
                if (g == "admins" && create_res && create_res->status == 200) {
                    auto new_group = json::parse(create_res->body);
                    if (new_group.contains("data") && new_group["data"].contains("createGroup")) {
                        admins_group_id = new_group["data"]["createGroup"]["id"].get<int>();
                    }
                }
            }
        }

        if (admins_group_id != -1) {
            json promo_payload = {
                {"query", "mutation AddUserToGroup($userId: String!, $groupId: Int!) { addUserToGroup(userId: $userId, groupId: $groupId) { ok } }"},
                {"variables", {{"userId", user}, {"groupId", admins_group_id}}}
            };
            lldap.Post("/api/graphql", headers, promo_payload.dump(), "application/json");
            fprintf(stderr, "[Landlord Cel-Tech-Serv] Admin access verified natively.\n");
        } else {
            fprintf(stderr, "[Landlord Cel-Tech-Serv] WARNING: 'admins' group ID could not be resolved.\n");
        }

        fprintf(stderr, "[Landlord Cel-Tech-Serv] Identity Bootstrap Sequence Complete.\n");
    }).detach(); 
}

class LandlordPlugin : public IOSLPlugin {
private:
    bool set_lldap_password(const std::string& admin_user, const std::string& admin_pass, const std::string& target_user, const std::string& new_pass) {
        auto escape = [](const std::string& str) {
            std::string res = "'";
            for (char c : str) { if (c == '\'') res += "'\\''"; else res += c; }
            res += "'"; return res;
        };

        std::string base_dn = "dc=osl,dc=net,dc=au";
        std::string admin_dn = "uid=" + admin_user + ",ou=people," + base_dn;
        std::string target_dn = "uid=" + target_user + ",ou=people," + base_dn;

        std::string cmd = "ldappasswd -x -H ldap://osl-identity:3890 -D " + escape(admin_dn) + 
                          " -w " + escape(admin_pass) + " -s " + escape(new_pass) + 
                          " " + escape(target_dn) + " 2>&1";
                          
        std::array<char, 128> buffer;
        std::string result;
        FILE* pipe = popen(cmd.c_str(), "r");
        if (!pipe) return false;
        while (fgets(buffer.data(), buffer.size(), pipe) != nullptr) result += buffer.data();
        int exit_code = WEXITSTATUS(pclose(pipe));
        return exit_code == 0;
    }

public:
    std::string get_plugin_name() override {
        return "Cel-Tech-Serv Pty Ltd - Landlord Identity Manager";
    }

    void pre_commit_hook(std::vector<LedgerLine>& transaction) override {}

    json execute(const std::string& command, const json& payload) override {
        
        if (command == "get_ui_extensions") {
            json response = json::object();
            
            // INJECTED HTML: Sidebar metadata and hidden hooks
            response["html"] = R"HTML(
                <div id='landlord-identity-header' class='mb-6 p-4 border-l-4 border-blue-600 bg-blue-50 dark:bg-blue-900/10'>
                    <div class='flex justify-between items-start'>
                        <div>
                            <h1 id='active-entity-display' class='text-lg font-black text-blue-800 dark:text-blue-400 uppercase tracking-tighter'>Loading Sovereign Identity...</h1>
                            <p id='active-entity-meta' class='text-[10px] text-gray-500 font-mono uppercase'></p>
                            <span id='user-role-badge' class='mt-2 inline-block px-2 py-0.5 rounded text-[9px] font-black uppercase tracking-widest'></span>
                        </div>
                        <div class='text-right'>
                            <span class='text-[9px] font-black text-blue-600/50 uppercase tracking-widest'>Sovereign Node</span><br>
                            <span class='text-[9px] font-bold text-gray-400 uppercase'>Cel-Tech-Serv Pty Ltd</span>
                        </div>
                    </div>
                </div>
            )HTML";

            // INJECTED JAVASCRIPT: The entire Tenancy Engine, safely registering with window.pluginHooks
            response["js"] = R"JS(
                window.pluginHooks = window.pluginHooks || {};
                
                window.landlordEntities = [];
                window.allSovereignUsers = []; 
                window.allUserMappings = [];

                // 1. CORE HOOK: App Initialisation
                window.pluginHooks.onAppInit = async function() {
                    await window.refreshLandlordContext();
                };

                // 2. CORE HOOK: Provide the expanded array of entity IDs a user is allowed to see
                window.pluginHooks.getExpandedContexts = function(activeContext) {
                    let validDivisions = [activeContext];
                    if (window.landlordEntities) {
                        const isRoot = window.landlordEntities.find(e => e.id === activeContext && (!e.parent_id || e.parent_id === ""));
                        if (isRoot) {
                            const children = window.landlordEntities.filter(e => e.parent_id === activeContext).map(e => e.id);
                            validDivisions.push(...children);
                        }
                    }
                    return validDivisions;
                };

                // 3. CORE HOOK: Translate Database Entity IDs into Beautiful Names for the Ledger View
                window.pluginHooks.getEntityName = function(id) {
                    if (window.landlordEntities) {
                        const matchedDiv = window.landlordEntities.find(d => d.id === id);
                        if (matchedDiv) return matchedDiv.name;
                    }
                    return null;
                };

                // 4. CORE HOOK: What to do when the user clicks the "User Management" sidebar button
                window.pluginHooks.onUserManagementShow = function() {
                    window.switchTenancyTab('users');
                    window.fetchActiveUsers();
                };

                // --- THE REST OF THE TENANCY JS ENGINE LIVES HERE, ISOLATED ---
                
                window.refreshIdentity = function() {
                    const divId = document.getElementById('osl-context-switcher')?.value || 'global';
                    fetch(`/api/plugin/au.com.celtechserv.landlord/get_user_context`, {
                        method: 'POST',
                        headers: { 'Content-Type': 'application/json' },
                        body: JSON.stringify({ entity_id: divId })
                    })
                    .then(res => res.json())
                    .then(data => {
                        const header = document.getElementById('active-entity-display');
                        const meta = document.getElementById('active-entity-meta');
                        const badge = document.getElementById('user-role-badge');
                        if (header && data.name) {
                            header.innerText = data.name;
                            meta.innerText = `ABN/ACN: ${data.acn || 'N/A'} | NODE: ${data.node_id || 'OSL-PRIMARY'}`;
                            badge.innerText = data.role || 'GUEST';
                            badge.className = "mt-2 inline-block px-2 py-0.5 rounded text-[9px] font-black uppercase tracking-widest ";
                            if (data.role === 'ADMINISTRATOR') badge.classList.add('bg-red-500', 'text-white');
                            else if (data.role === 'ACCOUNTANT') badge.classList.add('bg-green-500', 'text-white');
                            else badge.classList.add('bg-blue-500', 'text-white'); 
                        }
                    });
                };
                
                document.addEventListener('DOMContentLoaded', () => {
                    const switcher = document.getElementById('osl-context-switcher');
                    if (switcher) {
                        switcher.addEventListener('change', () => {
                            window.refreshIdentity();
                            if (typeof window.refreshLedger === 'function') window.refreshLedger(); 
                        });
                        setTimeout(window.refreshIdentity, 300);
                    }
                });

                window.switchTenancyTab = function(tabName) {
                    const tabs = ['users', 'root', 'trading'];
                    const activeClassList = ['text-blue-600', 'border-blue-600', 'bg-white', 'dark:bg-slate-900'];
                    const inactiveClassList = ['text-slate-500', 'border-transparent', 'bg-slate-50', 'dark:bg-slate-950'];

                    tabs.forEach(t => {
                        const btn = document.getElementById(`tab-btn-${t}`);
                        const content = document.getElementById(`tab-content-${t}`);
                        if (!btn || !content) return;

                        if (t === tabName) {
                            content.classList.remove('hidden');
                            btn.classList.add(...activeClassList);
                            btn.classList.remove(...inactiveClassList);
                        } else {
                            content.classList.add('hidden');
                            btn.classList.add(...inactiveClassList);
                            btn.classList.remove(...activeClassList);
                        }
                    });

                    if (tabName === 'trading') window.updateTradingNameDropdowns(); 
                };

                window.updateSlug = function(sourceId, targetId) {
                    const sourceText = document.getElementById(sourceId).value;
                    const targetElement = document.getElementById(targetId);
                    let slug = sourceText.toLowerCase().replace(/[^a-z0-9]+/g, '_').replace(/(^_|_$)/g, '');
                    targetElement.value = slug;
                };

                window.refreshLandlordContext = async function() {
                    const switcher = document.getElementById('osl-context-switcher');
                    const mgmtRootSelector = document.getElementById('mgmt-root-selector');
                    
                    const deactRootSelector = document.getElementById('deactivate-root-selector');
                    const manageTradeSelector = document.getElementById('manage-trade-selector');
                    const transferRootSelector = document.getElementById('transfer-root-selector');
                    
                    const deleteRootSelector = document.getElementById('delete-root-selector');
                    const deleteTradeSelector = document.getElementById('delete-trade-selector');
                    const directoryFilter = document.getElementById('directory-tenancy-filter');

                    try {
                        const res = await fetch('/api/plugin/au.com.celtechserv.landlord/get_user_entities', {
                            method: 'POST',
                            headers: { 'Content-Type': 'application/json' },
                            body: JSON.stringify({}) 
                        });
                        
                        if (res.ok) {
                            const data = await res.json();
                            if (data.entities) {
                                window.landlordEntities = data.entities;
                                
                                let switcherHtml = '';
                                let mgmtRootHtml = '<option value="">-- Select a Root Company --</option>';
                                
                                let deactRootHtml = '<option value="">-- Select Root to Deactivate --</option>';
                                let delRootHtml = '<option value="">-- Select Root to HARD DELETE --</option>';
                                let manageTradeHtml = '<option value="">-- Select Trading Name --</option>';
                                let delTradeHtml = '<option value="">-- Select Trading Name to HARD DELETE --</option>';
                                let transferRootHtml = '<option value="">-- Promote to Independent Master Root --</option>';
                                
                                let directoryFilterHtml = '<option value="all">-- Show All Users --</option><option value="unassigned">-- Show Unassigned Users --</option>';

                                const roots = data.entities.filter(e => !e.parent_id || e.parent_id === "");
                                
                                roots.forEach(root => {
                                    switcherHtml += `<optgroup label="${root.name.toUpperCase()}">`;
                                    switcherHtml += `<option value="${root.id}">ALL (${root.name})</option>`;
                                    
                                    mgmtRootHtml += `<option value="${root.id}">${root.name.toUpperCase()}</option>`;
                                    transferRootHtml += `<option value="${root.id}">${root.name.toUpperCase()}</option>`;
                                    
                                    directoryFilterHtml += `<option value="${root.id}">[ROOT] ${root.name.toUpperCase()}</option>`;

                                    if (root.id !== 'global') {
                                        deactRootHtml += `<option value="${root.id}">${root.name.toUpperCase()}</option>`;
                                        delRootHtml += `<option value="${root.id}">${root.name.toUpperCase()}</option>`;
                                    }
                                    
                                    const children = data.entities.filter(e => e.parent_id === root.id);
                                    children.forEach(child => {
                                        switcherHtml += `<option value="${child.id}">${child.name}</option>`;
                                        manageTradeHtml += `<option value="${child.id}">${child.name} (Under: ${root.name})</option>`;
                                        delTradeHtml += `<option value="${child.id}">${child.name} (Under: ${root.name})</option>`;
                                        directoryFilterHtml += `<option value="${child.id}">-- [TRADE] ${child.name}</option>`;
                                    });
                                    
                                    switcherHtml += `</optgroup>`;
                                });
                                
                                if (switcher) switcher.innerHTML = switcherHtml;
                                if (mgmtRootSelector) mgmtRootSelector.innerHTML = mgmtRootHtml;
                                if (deactRootSelector) deactRootSelector.innerHTML = deactRootHtml;
                                if (deleteRootSelector) deleteRootSelector.innerHTML = delRootHtml;
                                
                                if (manageTradeSelector) manageTradeSelector.innerHTML = manageTradeHtml;
                                if (deleteTradeSelector) deleteTradeSelector.innerHTML = delTradeHtml;
                                if (transferRootSelector) transferRootSelector.innerHTML = transferRootHtml;
                                if (directoryFilter) directoryFilter.innerHTML = directoryFilterHtml;
                                
                                if (typeof window.refreshIdentity === 'function') window.refreshIdentity();
                            }
                        }
                    } catch (err) {
                        console.error("[LANDLORD] Entity Fetch Failed:", err);
                    }
                };

                window.updateTradingNameDropdowns = function() {
                    const rootId = document.getElementById('mgmt-root-selector')?.value;
                    const tradeSelector = document.getElementById('mgmt-trading-selector');
                    if (!tradeSelector) return;
                    if (!rootId) { tradeSelector.innerHTML = '<option value="">Select a Root Company First</option>'; return; }

                    const children = window.landlordEntities.filter(e => e.parent_id === rootId);
                    if (children.length === 0) { tradeSelector.innerHTML = '<option value="">No Trading Names found under this Root</option>'; return; }

                    let html = '';
                    children.forEach(child => { html += `<option value="${child.id}">${child.name}</option>`; });
                    tradeSelector.innerHTML = html;
                };

                window.populateAccessDropdowns = function(users) {
                    const accSelector = document.getElementById('mgmt-accountant-selector');
                    const clientSelector = document.getElementById('mgmt-client-selector');
                    if (!accSelector || !clientSelector) return;

                    let accHtml = '';
                    let clientHtml = '';

                    users.forEach(user => {
                        if (user.groups && user.groups.some(g => String(g.id) === '1' || g.displayName === 'admins' || g.id === 'lldap_admin')) return;

                        let isAccountant = false;
                        if (user.groups && user.groups.some(g => g.displayName === 'accountants')) isAccountant = true;

                        if (isAccountant) { accHtml += `<option value="${user.id}">${user.id} (${user.displayName || 'No Name'})</option>`; } 
                        else { clientHtml += `<option value="${user.id}">${user.id} (${user.displayName || 'No Name'})</option>`; }
                    });

                    accSelector.innerHTML = accHtml || '<option value="">No Accountants Found</option>';
                    clientSelector.innerHTML = clientHtml || '<option value="">No Clients Found</option>';
                };

                window.submitRootCompany = async function() {
                    const payload = {
                        entity_id: document.getElementById('root-id').value.trim(),
                        business_name: document.getElementById('root-name').value.trim(),
                        acn_abn: document.getElementById('root-acn').value.trim(),
                        parent_id: "" 
                    };
                    await window.executeTenancyCreation(payload, 'root-tenancy-form');
                };

                window.submitTradingName = async function() {
                    const rootId = document.getElementById('mgmt-root-selector').value;
                    if (!rootId) { alert("You must select a Master Root Company first."); return; }

                    const payload = {
                        entity_id: document.getElementById('trade-id').value.trim(),
                        business_name: document.getElementById('trade-name').value.trim(),
                        acn_abn: document.getElementById('trade-acn').value.trim(),
                        parent_id: rootId 
                    };
                    await window.executeTenancyCreation(payload, null); 
                    document.getElementById('trade-id').value = '';
                    document.getElementById('trade-name').value = '';
                    document.getElementById('trade-acn').value = '';
                    window.updateTradingNameDropdowns(); 
                };

                window.executeTenancyCreation = async function(payload, formIdToReset) {
                    if (!payload.entity_id || !payload.business_name) { alert("Entity ID and Business/Company Name are required."); return; }
                    try {
                        const res = await fetch('/api/plugin/au.com.celtechserv.landlord/create_entity', {
                            method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify(payload)
                        });
                        const data = await res.json();
                        if (data.status === 'SUCCESS') {
                            alert(data.message);
                            if (formIdToReset) document.getElementById(formIdToReset).reset();
                            await window.refreshLandlordContext(); 
                        } else { alert("Error: " + (data.message || "Failed to create tenancy.")); }
                    } catch (err) { alert("Network error occurred while provisioning tenancy."); }
                };

                window.deactivateEntity = async function(type) {
                    const targetId = type === 'root' ? document.getElementById('deactivate-root-selector').value : document.getElementById('manage-trade-selector').value;
                    if (!targetId) { alert("Please select an entity to deactivate."); return; }
                    if (!confirm("Are you sure you want to deactivate this entity?\n\nHistorical ledger records will be strictly preserved, but it will be hidden from all active menus and dropdowns. If you are deactivating a Root Company, all attached Trading Names will be deactivated as well.")) return;
                    try {
                        const res = await fetch('/api/plugin/au.com.celtechserv.landlord/deactivate_entity', {
                            method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify({ target_id: targetId })
                        });
                        const data = await res.json();
                        if (data.status === 'SUCCESS') { alert(data.message); await window.refreshLandlordContext(); } 
                        else { alert("Error: " + data.message); }
                    } catch (e) { alert("Network error during deactivation."); }
                };

                window.hardDeleteEntity = async function(type) {
                    const targetSelector = type === 'root' ? 'delete-root-selector' : 'delete-trade-selector';
                    const conf1Selector = type === 'root' ? 'del-root-conf1' : 'del-trade-conf1';
                    const conf2Selector = type === 'root' ? 'del-root-conf2' : 'del-trade-conf2';

                    const targetId = document.getElementById(targetSelector).value;
                    const conf1 = document.getElementById(conf1Selector).value;
                    const conf2 = document.getElementById(conf2Selector).value;

                    if (!targetId) { alert("Please select an entity to delete."); return; }
                    if (conf1 !== "DELETE" || conf2 !== targetId) {
                        alert("Dual confirmation failed.\nYou must type 'DELETE' in the first box, and the exact Entity ID ('" + targetId + "') in the second box.");
                        return;
                    }
                    if (!confirm(`CRITICAL WARNING:\nYou are about to HARD DELETE '${targetId}'.\n\nIf this is a Root Company, all child Trading Names will also be erased recursively.\n\nThis will fail if any financial ledger entries are attached.\n\nProceed?`)) return;

                    try {
                        const res = await fetch('/api/plugin/au.com.celtechserv.landlord/delete_entity', {
                            method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify({ target_id: targetId, confirm_1: conf1, confirm_2: conf2 })
                        });
                        const data = await res.json();
                        if (data.status === 'SUCCESS') {
                            alert(data.message);
                            document.getElementById(conf1Selector).value = '';
                            document.getElementById(conf2Selector).value = '';
                            await window.refreshLandlordContext();
                        } else { alert("Error: " + data.message); }
                    } catch (e) { alert("Network error during hard deletion."); }
                };

                window.transferTradingName = async function() {
                    const targetId = document.getElementById('manage-trade-selector').value;
                    const newRootId = document.getElementById('transfer-root-selector').value;
                    if (!targetId) { alert("Please select a Trading Name to transfer."); return; }
                    try {
                        const res = await fetch('/api/plugin/au.com.celtechserv.landlord/transfer_entity', {
                            method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify({ target_id: targetId, new_parent_id: newRootId })
                        });
                        const data = await res.json();
                        if (data.status === 'SUCCESS') { alert(data.message); await window.refreshLandlordContext(); } 
                        else { alert("Error: " + data.message); }
                    } catch (e) { alert("Network error during transfer."); }
                };

                window.linkAccountantToRoot = async function() {
                    const targetEid = document.getElementById('mgmt-root-selector').value;
                    const targetUid = document.getElementById('mgmt-accountant-selector').value;
                    if (!targetEid) { alert("Please select a Master Root Company."); return; }
                    if (!targetUid) { alert("Please select an Accountant to assign."); return; }
                    await window.performLandlordMapping(targetUid, targetEid);
                };

                window.linkClientToTrading = async function() {
                    const targetEid = document.getElementById('mgmt-trading-selector').value;
                    const targetUid = document.getElementById('mgmt-client-selector').value;
                    if (!targetEid) { alert("Please ensure a Trading Name is selected. If none exist, create one first."); return; }
                    if (!targetUid) { alert("Please select a Client to assign."); return; }
                    await window.performLandlordMapping(targetUid, targetEid);
                };

                window.performLandlordMapping = async function(targetUid, targetEid) {
                    const payload = { target_uid: targetUid, target_eid: targetEid };
                    try {
                        const res = await fetch('/api/plugin/au.com.celtechserv.landlord/add_user_mapping', {
                            method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify(payload)
                        });
                        const data = await res.json();
                        if (data.status === 'success') { 
                            alert(data.message || "Sovereign Access Granted Successfully.");
                            await window.fetchActiveUsers(); 
                        } else { alert("Linking failed: " + (data.message || "Ensure you have proper privileges.")); }
                    } catch (err) { alert("Network error occurred while mapping user."); }
                };

                window.fetchActiveUsers = async function() {
                    try {
                        const listContainer = document.getElementById('user-list-container');
                        if (!listContainer) return;

                        const authRes = await fetch('/api/auth/me');
                        const authData = await authRes.json();
                        const currentUser = authData.user;
                        const currentGroups = JSON.stringify(authData.groups || []).toLowerCase();

                        const isCurrentAdmin = currentGroups.includes('admin');
                        const isCurrentAccountant = currentGroups.includes('accountant');
                        const isStandardUser = !isCurrentAdmin && !isCurrentAccountant;

                        const response = await fetch('/api/plugin/au.com.celtechserv.landlord/list_users', {
                            method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify({}) 
                        });
                        const data = await response.json();
                        
                        const mappingRes = await fetch('/api/plugin/au.com.celtechserv.landlord/get_all_mappings', {
                            method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify({}) 
                        });
                        const mappingData = await mappingRes.json();
                        if (mappingData.status === 'success') window.allUserMappings = mappingData.mappings;

                        if (response.ok && data.data && data.data.users) {
                            window.allSovereignUsers = data.data.users; 
                            window.applyUserFilter(currentUser, isCurrentAdmin, isCurrentAccountant, isStandardUser);
                            window.populateAccessDropdowns(window.allSovereignUsers);

                            const userForm = document.getElementById('user-form-container');
                            if (userForm) { userForm.style.display = isStandardUser ? 'none' : 'block'; }
                            
                            const tenancyContainer = document.getElementById('tenancy-management-container');
                            if (tenancyContainer) {
                                tenancyContainer.style.display = isStandardUser ? 'none' : 'block';
                                const tab2Btn = document.getElementById('tab-btn-root');
                                if (tab2Btn) tab2Btn.style.display = isCurrentAdmin ? 'block' : 'none';
                            }

                            document.querySelectorAll('.admin-only-block').forEach(el => {
                                el.style.display = isCurrentAdmin ? 'block' : 'none';
                            });
                        } else {
                            listContainer.innerHTML = `<p class="text-red-500 font-bold">Backend Error: ${data.message || data.error || "Unknown"}</p>`;
                        }
                    } catch (err) {
                        const listContainer = document.getElementById('user-list-container');
                        if (listContainer) listContainer.innerHTML = `<p class="text-red-500 font-bold bg-red-900/20 p-4 border border-red-500 rounded">Application UI Exception.</p>`;
                    }
                };

                window.applyUserFilter = function(currentUser, isCurrentAdmin, isCurrentAccountant, isStandardUser) {
                    const listContainer = document.getElementById('user-list-container');
                    const filterEid = document.getElementById('directory-tenancy-filter')?.value || 'all';

                    let usersToRender = window.allSovereignUsers;

                    if (filterEid === 'unassigned') {
                        usersToRender = window.allSovereignUsers.filter(u => { return !window.allUserMappings.some(m => m.uid === u.id); });
                    } else if (filterEid !== 'all') {
                        usersToRender = window.allSovereignUsers.filter(u => { return window.allUserMappings.some(m => m.uid === u.id && m.eid === filterEid); });
                    }

                    const tableHtml = window.renderUserTable(usersToRender, currentUser, isCurrentAdmin, isCurrentAccountant, isStandardUser);
                    listContainer.innerHTML = tableHtml;
                };

                window.renderUserTable = function(users, currentUser, isCurrentAdmin, isCurrentAccountant, isStandardUser) {
                    let tableHtml = `
                        <table class="w-full text-left text-[11px] border-collapse">
                            <thead>
                                <tr class="border-b border-slate-200 dark:border-slate-800 text-blue-600 dark:text-blue-400 uppercase font-black tracking-wider">
                                    <th class="py-3 px-2">Username</th>
                                    <th class="py-3 px-2">Display Name</th>
                                    <th class="py-3 px-2">System Role</th>
                                    <th class="py-3 px-2">Mapped Tenancies</th>
                                    <th class="py-3 px-2 text-center">Action</th>
                                </tr>
                            </thead>
                            <tbody>`;
                    
                    if (users.length === 0) return tableHtml + `<tr><td colspan="5" class="py-4 px-2 text-center text-slate-500 italic">No users found for this filter.</td></tr></tbody></table>`;

                    users.forEach(user => {
                        if (user.groups && Array.isArray(user.groups) && user.groups.some(g => String(g.id) === '1' || g.id === 'lldap_admin')) return; 

                        let userType = '<span class="text-slate-500 font-bold">[UNASSIGNED]</span>';
                        let targetRole = 'user'; 
                        let targetRoleId = 'clients'; 
                        let primaryGroup = null;

                        if (user.groups && Array.isArray(user.groups)) {
                            primaryGroup = user.groups.find(g => {
                                const gName = String(g.displayName || g.id).toLowerCase();
                                return gName.includes('admin') || gName.includes('accountant') || gName.includes('client') || gName.includes('user');
                            });
                        }

                        if (primaryGroup) {
                            const gName = String(primaryGroup.displayName || primaryGroup.id).toLowerCase();
                            let colour = 'text-slate-500 dark:text-slate-400 font-bold'; 
                            targetRoleId = String(primaryGroup.displayName || primaryGroup.id).toLowerCase();
                            
                            if (gName.includes('admin')) {
                                colour = 'text-blue-600 dark:text-blue-400 font-black tracking-wide';
                                targetRole = 'admin';
                            } else if (gName.includes('accountant')) {
                                colour = 'text-green-600 dark:text-green-500 font-bold';
                                targetRole = 'accountant';
                            }
                            userType = `<span class="${colour}">[${(primaryGroup.displayName || primaryGroup.id).toString().toUpperCase()}]</span>`;
                        }

                        if (!isCurrentAdmin) {
                            if (isCurrentAccountant) { if (user.id !== currentUser && targetRole !== 'user') return; } 
                            else { if (user.id !== currentUser) return; }
                        }

                        let userTenancies = window.allUserMappings.filter(m => m.uid === user.id).map(m => m.eid);
                        let tenancyBadges = '';
                        if (userTenancies.length === 0) {
                            tenancyBadges = '<span class="text-slate-400 dark:text-slate-600 italic text-[9px]">No Tenancy Access</span>';
                        } else {
                            userTenancies.forEach(eid => {
                                let entityName = eid;
                                if (window.landlordEntities) {
                                    let matchedEntity = window.landlordEntities.find(e => e.id === eid);
                                    if (matchedEntity) entityName = matchedEntity.name;
                                }
                                tenancyBadges += `<span class="bg-blue-100 dark:bg-slate-800 text-blue-800 dark:text-slate-300 border border-blue-200 dark:border-slate-700 px-1.5 py-0.5 rounded text-[8px] font-bold mr-1 mb-1 inline-block shadow-sm uppercase">${entityName}</span>`;
                            });
                        }

                        tableHtml += `
                            <tr class="border-b border-slate-200 dark:border-slate-800 hover:bg-blue-50 dark:hover:bg-blue-900/10 cursor-pointer transition-colors" 
                                data-id="${user.id}" data-email="${user.email}" data-display="${user.displayName || ''}" 
                                data-first="${user.firstName || ''}" data-last="${user.lastName || ''}" data-roleid="${targetRoleId}"
                                onclick="editSovereignUser(this.dataset)">
                                <td class="py-3 px-2 font-black text-slate-700 dark:text-slate-300">${user.id}</td>
                                <td class="py-3 px-2 text-slate-500 dark:text-slate-400 font-bold">${user.displayName || '-'}</td>
                                <td class="py-3 px-2">${userType}</td>
                                <td class="py-3 px-2 max-w-[200px] flex-wrap">${tenancyBadges}</td>
                                <td class="py-3 px-2 text-center text-blue-600 font-black tracking-widest text-[9px]">EDIT</td>
                            </tr>`;
                    });
                    
                    return tableHtml + `</tbody></table>`;
                };

                window.editSovereignUser = function(data) {
                    document.getElementById('edit-mode').value = 'true';
                    document.getElementById('new-username').value = data.id;
                    document.getElementById('new-username').readOnly = true; 
                    document.getElementById('new-email').value = data.email;
                    document.getElementById('new-displayname').value = data.display;
                    document.getElementById('new-firstname').value = data.first;
                    document.getElementById('new-lastname').value = data.last;
                    document.getElementById('new-password').value = ''; 
                    
                    const roleSelect = document.getElementById('new-role');
                    if(roleSelect) {
                        const optionExists = Array.from(roleSelect.options).some(opt => opt.value === data.roleid);
                        roleSelect.value = optionExists ? data.roleid : 'clients';
                    }
                    
                    const submitBtn = document.getElementById('user-submit-btn');
                    submitBtn.innerText = 'Update User: ' + data.id;
                    submitBtn.classList.replace('bg-blue-600', 'bg-orange-600');
                    
                    document.getElementById('user-cancel-btn').classList.remove('hidden');
                    document.getElementById('user-msg').innerHTML = `<span class="text-orange-600 dark:text-orange-400 font-bold italic">Editing User: ${data.id}</span>`;
                    
                    if (data.roleid === 'accountants') {
                        const accSel = document.getElementById('mgmt-accountant-selector');
                        if (accSel) accSel.value = data.id;
                    } else {
                        const clientSel = document.getElementById('mgmt-client-selector');
                        if (clientSel) clientSel.value = data.id;
                    }
                    window.scrollTo({ top: 0, behavior: 'smooth' });
                };

                window.cancelEdit = function() {
                    document.getElementById('user-form').reset();
                    document.getElementById('edit-mode').value = 'false';
                    document.getElementById('new-username').readOnly = false;
                    
                    const submitBtn = document.getElementById('user-submit-btn');
                    submitBtn.innerText = 'Add User';
                    submitBtn.classList.replace('bg-orange-600', 'bg-blue-600');
                    
                    document.getElementById('user-cancel-btn').classList.add('hidden');
                    document.getElementById('user-msg').innerHTML = '';
                };

                window.submitIdentity = async function() {
                    const isEdit = document.getElementById('edit-mode').value === 'true';
                    const endpoint = isEdit ? '/api/plugin/au.com.celtechserv.landlord/update_user' : '/api/plugin/au.com.celtechserv.landlord/add_user';
                    
                    const payload = {
                        username: document.getElementById('new-username').value,
                        email: document.getElementById('new-email').value,
                        displayName: document.getElementById('new-displayname').value,
                        firstName: document.getElementById('new-firstname').value,
                        lastName: document.getElementById('new-lastname').value,
                        password: document.getElementById('new-password').value,
                        roleId: document.getElementById('new-role').value 
                    };

                    if (!isEdit && !payload.password) {
                        document.getElementById('user-msg').innerHTML = '<span class="text-red-500 font-bold">Error: Password is required for new users.</span>';
                        return;
                    }

                    const msgDiv = document.getElementById('user-msg');
                    msgDiv.innerHTML = '<span class="text-blue-500 font-bold">Processing request...</span>';

                    try {
                        const response = await fetch(endpoint, {
                            method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify(payload)
                        });
                        const data = await response.json();
                        
                        if (response.ok && data.status === "SUCCESS") {
                            msgDiv.innerHTML = `<span class="text-green-600 font-bold">Success: ${data.message}</span>`;
                            window.cancelEdit(); 
                            window.fetchActiveUsers();
                        } else {
                            msgDiv.innerHTML = `<span class="text-red-500 font-bold">Error: ${data.message || data.error || "Unknown Error"}</span>`;
                        }
                    } catch (err) { msgDiv.innerHTML = '<span class="text-red-500 font-bold">Connection Error: Failed to update user.</span>'; }
                };

            )JS";
            return response;
        }

        if (command == "get_user_management_ui") {
            json response = json::object();
            // UNIFIED IDENTITY & ACCESS DASHBOARD
            response["html"] = R"HTML(
                <div class="bg-white dark:bg-slate-900 border border-slate-200 dark:border-slate-800 rounded-lg mt-4 overflow-hidden" id="tenancy-management-container">
                    
                    <div class="flex border-b border-slate-200 dark:border-slate-800 bg-slate-50 dark:bg-slate-950">
                        <button onclick="switchTenancyTab('users')" id="tab-btn-users" class="flex-1 py-4 text-[11px] font-black uppercase tracking-widest text-blue-600 border-b-2 border-blue-600 bg-white dark:bg-slate-900 transition-colors">1. User Directory</button>
                        <button onclick="switchTenancyTab('root')" id="tab-btn-root" class="flex-1 py-4 text-[11px] font-black uppercase tracking-widest text-slate-500 hover:text-slate-700 dark:hover:text-slate-300 border-b-2 border-transparent transition-colors">2. Root Legal Entities</button>
                        <button onclick="switchTenancyTab('trading')" id="tab-btn-trading" class="flex-1 py-4 text-[11px] font-black uppercase tracking-widest text-slate-500 hover:text-slate-700 dark:hover:text-slate-300 border-b-2 border-transparent transition-colors">3. Trading Names & Access</button>
                    </div>

                    <div id="tab-content-users" class="p-6">
                        
                        <div id="user-form-container" class="mb-8 p-6 bg-slate-50 dark:bg-slate-950 rounded border border-slate-200 dark:border-slate-800">
                            <h3 class="text-blue-600 dark:text-blue-400 font-bold mb-4 uppercase tracking-wider text-sm">Add / Edit Identity</h3>
                            <form id="user-form" onsubmit="event.preventDefault(); submitIdentity();" class="space-y-4">
                                <input type="hidden" id="edit-mode" value="false">
                                <div class="grid grid-cols-1 md:grid-cols-2 gap-4">
                                    <div><label class="block text-[10px] font-black uppercase mb-1 text-gray-500 tracking-widest">Username</label><input type="text" id="new-username" class="w-full bg-white dark:bg-slate-900 border border-gray-300 dark:border-slate-700 p-2 text-sm rounded shadow-sm" required></div>
                                    <div><label class="block text-[10px] font-black uppercase mb-1 text-gray-500 tracking-widest">Email</label><input type="email" id="new-email" class="w-full bg-white dark:bg-slate-900 border border-gray-300 dark:border-slate-700 p-2 text-sm rounded shadow-sm" required></div>
                                    <div><label class="block text-[10px] font-black uppercase mb-1 text-gray-500 tracking-widest">Display Name</label><input type="text" id="new-displayname" class="w-full bg-white dark:bg-slate-900 border border-gray-300 dark:border-slate-700 p-2 text-sm rounded shadow-sm" required></div>
                                    <div><label class="block text-[10px] font-black uppercase mb-1 text-gray-500 tracking-widest">Password (Leave blank if editing)</label><input type="password" id="new-password" class="w-full bg-white dark:bg-slate-900 border border-gray-300 dark:border-slate-700 p-2 text-sm rounded shadow-sm"></div>
                                    <div><label class="block text-[10px] font-black uppercase mb-1 text-gray-500 tracking-widest">First Name</label><input type="text" id="new-firstname" class="w-full bg-white dark:bg-slate-900 border border-gray-300 dark:border-slate-700 p-2 text-sm rounded shadow-sm"></div>
                                    <div><label class="block text-[10px] font-black uppercase mb-1 text-gray-500 tracking-widest">Last Name</label><input type="text" id="new-lastname" class="w-full bg-white dark:bg-slate-900 border border-gray-300 dark:border-slate-700 p-2 text-sm rounded shadow-sm"></div>
                                    <div class="md:col-span-2">
                                        <label class="block text-[10px] font-black uppercase mb-1 text-gray-500 tracking-widest">System Role</label>
                                        <select id="new-role" class="w-full bg-white dark:bg-slate-900 border border-gray-300 dark:border-slate-700 p-2 text-sm rounded shadow-sm">
                                            <option value="clients">Standard Client</option>
                                            <option value="accountants">Accountant</option>
                                            <option value="admins">System Admin</option>
                                        </select>
                                    </div>
                                </div>
                                <div class="flex items-center space-x-4 mt-6 pt-4 border-t border-gray-200 dark:border-slate-800">
                                    <button type="submit" id="user-submit-btn" class="bg-blue-600 hover:bg-blue-700 text-white font-bold py-2 px-6 rounded text-sm shadow-md">Save Identity</button>
                                    <button type="button" id="user-cancel-btn" onclick="cancelEdit()" class="hidden bg-gray-600 hover:bg-gray-700 text-white font-bold py-2 px-6 rounded text-sm shadow-md">Cancel</button>
                                    <div id="user-msg" class="text-sm font-mono tracking-wide"></div>
                                </div>
                            </form>
                        </div>

                        <div class="mb-4 flex items-center justify-between bg-slate-50 dark:bg-slate-950 p-4 rounded border border-slate-200 dark:border-slate-800">
                            <div>
                                <h3 class="text-blue-600 dark:text-blue-400 font-black uppercase tracking-wider text-xs">Active Directory</h3>
                                <p class="text-[9px] text-slate-500 uppercase font-bold mt-1">Filter users by assigned Tenancy.</p>
                            </div>
                            <div class="w-1/2">
                                <select id="directory-tenancy-filter" onchange="applyUserFilter()" class="w-full bg-white dark:bg-slate-900 border border-slate-300 dark:border-slate-700 p-2 text-xs rounded font-bold shadow-sm">
                                    <option value="all">-- Show All Users --</option>
                                </select>
                            </div>
                        </div>
                        <div id="user-list-container"></div>
                    </div>

                    <div id="tab-content-root" class="hidden p-6">
                        <div class="max-w-2xl">
                            <h3 class="text-green-600 dark:text-green-500 font-black uppercase text-xs tracking-widest mb-2">Create Root Legal Entity</h3>
                            <p class="text-[10px] text-slate-500 mb-6 uppercase">Define the Master Company, Partnership, or Sole Trader.</p>
                            
                            <form id="root-tenancy-form" onsubmit="event.preventDefault(); submitRootCompany();" class="space-y-4">
                                <div class="grid grid-cols-1 md:grid-cols-2 gap-4">
                                    <div class="md:col-span-2">
                                        <label class="block text-[9px] font-bold text-slate-400 uppercase">Legal Name (Company, Partnership, or Sole Trader)</label>
                                        <input type="text" id="root-name" class="w-full bg-slate-50 dark:bg-slate-950 border border-slate-200 dark:border-slate-800 p-2 text-xs rounded" oninput="updateSlug('root-name', 'root-id')" required>
                                    </div>
                                    <div>
                                        <label class="block text-[9px] font-bold text-slate-400 uppercase">ABN / ACN</label>
                                        <input type="text" id="root-acn" class="w-full bg-slate-50 dark:bg-slate-950 border border-slate-200 dark:border-slate-800 p-2 text-xs rounded" required>
                                    </div>
                                    <div>
                                        <label class="block text-[9px] font-bold text-slate-400 uppercase text-blue-500">Database System ID (Auto-generated)</label>
                                        <input type="text" id="root-id" class="w-full bg-blue-50/50 dark:bg-slate-900 border border-blue-200 dark:border-slate-700 p-2 text-xs rounded text-blue-600 font-mono" required>
                                        <p class="text-[8px] text-slate-400 mt-1 italic">Type 'global' here manually if overwriting the default Sovereign vault.</p>
                                    </div>
                                </div>
                                <button type="submit" class="w-full mt-4 bg-green-600 hover:bg-green-500 text-white font-black py-2 rounded text-[10px] uppercase tracking-widest shadow-lg">Provision Root Entity</button>
                            </form>

                            <div class="mt-8 pt-6 border-t border-slate-200 dark:border-slate-800">
                                <h3 class="text-orange-600 dark:text-orange-500 font-black uppercase text-xs tracking-widest mb-2">Deactivate Root Entity (Soft Delete)</h3>
                                <p class="text-[9px] text-slate-500 mb-4 uppercase">Admins & Accountants. Hides the entity & child books. Ledger data preserved.</p>
                                <div class="flex gap-2">
                                    <select id="deactivate-root-selector" class="flex-1 bg-white dark:bg-slate-950 border border-slate-200 dark:border-slate-800 p-2 text-xs rounded font-bold shadow-sm"></select>
                                    <button onclick="deactivateEntity('root')" class="bg-orange-600 hover:bg-orange-500 text-white font-black px-4 py-2 rounded text-[10px] uppercase tracking-widest shadow-md">Deactivate</button>
                                </div>
                            </div>

                            <div class="admin-only-block mt-4 pt-4 border-t border-red-500/30 hidden">
                                <h3 class="text-red-600 dark:text-red-500 font-black uppercase text-xs tracking-widest mb-2">Danger Zone: Hard Delete (Recursive)</h3>
                                <p class="text-[9px] text-slate-500 mb-4 uppercase font-bold text-red-400">Admins Only. Erases Root AND Trading Names. Fails if ledger entries exist.</p>
                                <div class="grid grid-cols-1 md:grid-cols-4 gap-2 items-end bg-red-50 dark:bg-red-900/10 p-3 rounded border border-red-200 dark:border-red-900/50">
                                    <div class="md:col-span-1">
                                        <label class="block text-[8px] font-black text-slate-500 uppercase mb-1">Select Target</label>
                                        <select id="delete-root-selector" class="w-full bg-white dark:bg-slate-950 border border-red-300 dark:border-red-800 p-2 text-xs rounded shadow-sm"></select>
                                    </div>
                                    <div class="md:col-span-1">
                                        <label class="block text-[8px] font-black text-slate-500 uppercase mb-1">Type 'DELETE'</label>
                                        <input type="text" id="del-root-conf1" class="w-full bg-white dark:bg-slate-950 border border-red-300 dark:border-red-800 p-2 text-xs rounded" placeholder="DELETE">
                                    </div>
                                    <div class="md:col-span-1">
                                        <label class="block text-[8px] font-black text-slate-500 uppercase mb-1">Type Entity ID</label>
                                        <input type="text" id="del-root-conf2" class="w-full bg-white dark:bg-slate-950 border border-red-300 dark:border-red-800 p-2 text-xs rounded" placeholder="e.g. cel_tech_serv">
                                    </div>
                                    <div class="md:col-span-1 flex items-end">
                                        <button onclick="hardDeleteEntity('root')" class="w-full bg-red-600 hover:bg-red-500 text-white font-black py-2 rounded text-[10px] uppercase tracking-widest shadow-md">Erase Entity</button>
                                    </div>
                                </div>
                            </div>

                        </div>
                    </div>

                    <div id="tab-content-trading" class="hidden p-6">
                        
                        <div class="mb-8 p-4 bg-slate-50 dark:bg-slate-950 border border-slate-200 dark:border-slate-800 rounded">
                            <label class="block text-[11px] font-black text-blue-600 dark:text-blue-400 uppercase tracking-widest mb-2">Step 1: Select Master Root Entity</label>
                            <select id="mgmt-root-selector" onchange="updateTradingNameDropdowns()" class="w-full bg-white dark:bg-slate-900 border border-slate-300 dark:border-slate-700 p-2 text-sm rounded font-bold shadow-sm"></select>
                        </div>

                        <div class="grid grid-cols-1 xl:grid-cols-3 gap-8">
                            
                            <div class="xl:col-span-1">
                                <h3 class="text-orange-600 dark:text-orange-500 font-black uppercase text-xs tracking-widest mb-4">Step 2: Add Trading Name</h3>
                                <form onsubmit="event.preventDefault(); submitTradingName();" class="space-y-4">
                                    <div>
                                        <label class="block text-[9px] font-bold text-slate-400 uppercase">Trading Name (e.g., Alfonso's Gelateria)</label>
                                        <input type="text" id="trade-name" class="w-full bg-slate-50 dark:bg-slate-950 border border-slate-200 dark:border-slate-800 p-2 text-xs rounded" oninput="updateSlug('trade-name', 'trade-id')" required>
                                    </div>
                                    <div>
                                        <label class="block text-[9px] font-bold text-slate-400 uppercase">ABN (If different from Root)</label>
                                        <input type="text" id="trade-acn" class="w-full bg-slate-50 dark:bg-slate-950 border border-slate-200 dark:border-slate-800 p-2 text-xs rounded">
                                    </div>
                                    <div>
                                        <label class="block text-[9px] font-bold text-slate-400 uppercase text-orange-500">Database System ID</label>
                                        <input type="text" id="trade-id" class="w-full bg-orange-50/50 dark:bg-slate-900 border border-orange-200 dark:border-slate-700 p-2 text-xs rounded text-orange-600 font-mono" required>
                                    </div>
                                    <button type="submit" class="w-full bg-orange-600 hover:bg-orange-500 text-white font-black py-2 rounded text-[10px] uppercase tracking-widest shadow-lg">Create Trading Name</button>
                                </form>
                            </div>

                            <div class="xl:col-span-2 space-y-6">
                                
                                <div class="p-4 border-l-4 border-blue-500 bg-blue-50/50 dark:bg-blue-900/10 rounded">
                                    <h3 class="text-blue-600 dark:text-blue-400 font-black uppercase text-xs tracking-widest mb-1">Grant Accountant Access</h3>
                                    <p class="text-[9px] text-slate-500 mb-4 uppercase">Accountants inherit access to the Root Company AND all its Trading Names.</p>
                                    <div class="flex gap-2">
                                        <select id="mgmt-accountant-selector" class="flex-1 bg-white dark:bg-slate-950 border border-slate-200 dark:border-slate-800 p-2 text-xs rounded"><option value="">No Accountants Found</option></select>
                                        <button onclick="linkAccountantToRoot()" class="bg-blue-600 hover:bg-blue-500 text-white font-black px-4 py-2 rounded text-[10px] uppercase tracking-widest shadow-md">Assign to Root</button>
                                    </div>
                                </div>

                                <div class="p-4 border-l-4 border-purple-500 bg-purple-50/50 dark:bg-purple-900/10 rounded">
                                    <h3 class="text-purple-600 dark:text-purple-400 font-black uppercase text-xs tracking-widest mb-1">Grant Client Access</h3>
                                    <p class="text-[9px] text-slate-500 mb-4 uppercase">Clients ONLY see the specific Trading Name books they are assigned to.</p>
                                    <div class="flex gap-2 mb-2">
                                        <div class="flex-1"><label class="block text-[9px] font-bold text-slate-400 uppercase">Target Trading Name Book</label><select id="mgmt-trading-selector" class="w-full bg-white dark:bg-slate-950 border border-slate-200 dark:border-slate-800 p-2 text-xs rounded"><option value="">Select a Root Company First</option></select></div>
                                    </div>
                                    <div class="flex gap-2">
                                        <select id="mgmt-client-selector" class="flex-1 bg-white dark:bg-slate-950 border border-slate-200 dark:border-slate-800 p-2 text-xs rounded"><option value="">No Clients Found</option></select>
                                        <button onclick="linkClientToTrading()" class="bg-purple-600 hover:bg-purple-500 text-white font-black px-4 py-2 rounded text-[10px] uppercase tracking-widest shadow-md">Assign to Book</button>
                                    </div>
                                </div>
                            </div>

                            <div class="mt-4 pt-6 border-t border-slate-200 dark:border-slate-800 xl:col-span-3">
                                <h3 class="text-indigo-600 dark:text-indigo-400 font-black uppercase text-xs tracking-widest mb-2">Manage Trading Names</h3>
                                <div class="grid grid-cols-1 md:grid-cols-3 gap-4 bg-slate-50 dark:bg-slate-950 p-4 rounded border border-slate-200 dark:border-slate-800">
                                    <div>
                                        <label class="block text-[9px] font-bold text-slate-400 uppercase mb-1">Select Trading Name</label>
                                        <select id="manage-trade-selector" class="w-full bg-white dark:bg-slate-900 border border-slate-300 dark:border-slate-700 p-2 text-xs rounded font-bold shadow-sm"></select>
                                    </div>
                                    <div>
                                        <label class="block text-[9px] font-bold text-slate-400 uppercase mb-1">Transfer to New Root</label>
                                        <div class="flex gap-2">
                                            <select id="transfer-root-selector" class="flex-1 bg-white dark:bg-slate-900 border border-slate-300 dark:border-slate-700 p-2 text-xs rounded shadow-sm"></select>
                                            <button onclick="transferTradingName()" class="bg-indigo-600 hover:bg-indigo-500 text-white font-black px-4 py-2 rounded text-[10px] uppercase tracking-widest shadow-md">Transfer</button>
                                        </div>
                                    </div>
                                    <div>
                                        <label class="block text-[9px] font-bold text-slate-400 uppercase mb-1">Soft Delete Entity</label>
                                        <button onclick="deactivateEntity('trade')" class="w-full bg-orange-600 hover:bg-orange-500 text-white font-black px-4 py-2 rounded text-[10px] uppercase tracking-widest shadow-md">Deactivate Trading Name</button>
                                    </div>
                                </div>
                                
                                <div class="admin-only-block mt-4 pt-4 border-t border-red-500/30 hidden">
                                    <h3 class="text-red-600 dark:text-red-500 font-black uppercase text-xs tracking-widest mb-2">Danger Zone: Hard Delete</h3>
                                    <div class="grid grid-cols-1 md:grid-cols-4 gap-2 items-end bg-red-50 dark:bg-red-900/10 p-3 rounded border border-red-200 dark:border-red-900/50">
                                        <div class="md:col-span-1">
                                            <label class="block text-[8px] font-black text-slate-500 uppercase mb-1">Select Target</label>
                                            <select id="delete-trade-selector" class="w-full bg-white dark:bg-slate-950 border border-red-300 dark:border-red-800 p-2 text-xs rounded shadow-sm"></select>
                                        </div>
                                        <div class="md:col-span-1">
                                            <label class="block text-[8px] font-black text-slate-500 uppercase mb-1">Type 'DELETE'</label>
                                            <input type="text" id="del-trade-conf1" class="w-full bg-white dark:bg-slate-950 border border-red-300 dark:border-red-800 p-2 text-xs rounded" placeholder="DELETE">
                                        </div>
                                        <div class="md:col-span-1">
                                            <label class="block text-[8px] font-black text-slate-500 uppercase mb-1">Type Entity ID</label>
                                            <input type="text" id="del-trade-conf2" class="w-full bg-white dark:bg-slate-950 border border-red-300 dark:border-red-800 p-2 text-xs rounded" placeholder="e.g. alfonsos_gelato">
                                        </div>
                                        <div class="md:col-span-1 flex items-end">
                                            <button onclick="hardDeleteEntity('trade')" class="w-full bg-red-600 hover:bg-red-500 text-white font-black py-2 rounded text-[10px] uppercase tracking-widest shadow-md">Erase Entity</button>
                                        </div>
                                    </div>
                                </div>
                            </div>
                        </div>
                    </div>
                </div>
            )HTML";
            return response;
        }

        if (command == "get_user_entities") {
            try {
                std::string current_groups = payload.value("user_groups", "");
                std::string current_user = payload.value("user_id", "unknown");
                pqxx::connection C(std::getenv("OSL_DB_CONN"));
                pqxx::work W(C);

                pqxx::result R;
                if (current_groups.find("admins") != std::string::npos || current_groups.find("lldap_admin") != std::string::npos) {
                    R = W.exec("SELECT entity_id, business_name, parent_id FROM landlord_entities WHERE is_active = true ORDER BY parent_id NULLS FIRST, business_name ASC");
                } else {
                    R = W.exec("SELECT e.entity_id, e.business_name, e.parent_id FROM landlord_entities e WHERE e.is_active = true AND (e.entity_id IN (SELECT entity_id FROM landlord_user_mapping WHERE lldap_uid = " + W.quote(current_user) + ") OR e.parent_id IN (SELECT entity_id FROM landlord_user_mapping WHERE lldap_uid = " + W.quote(current_user) + "))");
                }

                json entities = json::array();
                for (auto row : R) {
                    entities.push_back({
                        {"id", row[0].as<std::string>()}, 
                        {"name", row[1].as<std::string>()},
                        {"parent_id", row[2].is_null() ? "" : row[2].as<std::string>()} 
                    });
                }
                return {{"entities", entities}};
            } catch (const std::exception &e) { return {{"status", "error"}}; }
        }

        if (command == "get_user_context") {
            try {
                std::string current_groups = payload.value("user_groups", "");
                std::string target_div = payload.value("entity_id", "global");
                pqxx::connection C(std::getenv("OSL_DB_CONN"));
                pqxx::work W(C);

                std::string role = "CLIENT";
                if (current_groups.find("admins") != std::string::npos || current_groups.find("lldap_admin") != std::string::npos) role = "ADMINISTRATOR";
                else if (current_groups.find("accountants") != std::string::npos) role = "ACCOUNTANT";

                pqxx::result R = W.exec("SELECT business_name, acn_abn FROM landlord_entities WHERE entity_id = " + W.quote(target_div));
                if (R.empty()) return {{"name", "Sovereign Vault"}, {"role", role}, {"acn", "Node Unconfigured"}, {"node_id", "CTS-OSL-01"}};

                return {{"name", R[0][0].as<std::string>()}, {"acn", R[0][1].as<std::string>()}, {"role", role}, {"node_id", "CTS-OSL-01"}};
            } catch (const std::exception &e) { return {{"status", "error"}}; }
        }

        if (command == "get_all_mappings") {
            std::string current_groups = payload.value("user_groups", "");
            if (current_groups.find("admins") == std::string::npos && current_groups.find("lldap_admin") == std::string::npos && current_groups.find("accountants") == std::string::npos) {
                return {{"status", "error"}, {"message", "Access Denied."}};
            }
            try {
                pqxx::connection C(std::getenv("OSL_DB_CONN"));
                pqxx::work W(C);
                pqxx::result R = W.exec("SELECT lldap_uid, entity_id FROM landlord_user_mapping");
                json mappings = json::array();
                for (auto row : R) {
                    mappings.push_back({
                        {"uid", row[0].as<std::string>()},
                        {"eid", row[1].as<std::string>()}
                    });
                }
                return {{"status", "success"}, {"mappings", mappings}};
            } catch (const std::exception &e) { return {{"status", "error"}}; }
        }

        if (command == "init_sub_db") {
            try {
                pqxx::connection C(std::getenv("OSL_DB_CONN"));
                pqxx::work W(C);
                
                W.exec("CREATE TABLE IF NOT EXISTS landlord_entities (entity_id VARCHAR(50) PRIMARY KEY, business_name TEXT NOT NULL, acn_abn VARCHAR(20), is_active BOOLEAN DEFAULT TRUE);");
                W.exec("ALTER TABLE landlord_entities ADD COLUMN IF NOT EXISTS parent_id VARCHAR(50) REFERENCES landlord_entities(entity_id);");
                W.exec("CREATE TABLE IF NOT EXISTS landlord_user_mapping (id SERIAL PRIMARY KEY, lldap_uid VARCHAR(100) NOT NULL, entity_id VARCHAR(50) REFERENCES landlord_entities(entity_id), UNIQUE(lldap_uid, entity_id));");
                
                W.exec("INSERT INTO landlord_entities (entity_id, business_name, acn_abn) VALUES ('global', 'Sovereign Global Vault', 'N/A') ON CONFLICT (entity_id) DO NOTHING;");
                W.commit();
                return {{"status", "success"}, {"message", "Schema Verified & Clean Slate Booted."}};
            } catch (const std::exception &e) { return {{"status", "error"}}; }
        }

        if (command == "deactivate_entity") {
            std::string current_groups = payload.value("user_groups", "");
            if (current_groups.find("admins") == std::string::npos && current_groups.find("lldap_admin") == std::string::npos && current_groups.find("accountants") == std::string::npos) {
                return {{"status", "error"}, {"message", "Action Requires Administrator or Accountant Privileges."}};
            }

            try {
                std::string target_id = payload.at("target_id").get<std::string>();
                if (target_id == "global") return {{"status", "error"}, {"message", "Cannot deactivate the master global vault."}};
                
                pqxx::connection C(std::getenv("OSL_DB_CONN"));
                pqxx::work W(C);
                W.exec("UPDATE landlord_entities SET is_active = false WHERE entity_id = " + W.quote(target_id) + " OR parent_id = " + W.quote(target_id));
                W.commit();
                return {{"status", "SUCCESS"}, {"message", "Entity deactivated successfully. Historical ledger records preserved."}};
            } catch (const std::exception &e) {
                return {{"status", "error"}, {"message", "Database error."}};
            }
        }

        if (command == "delete_entity") {
            std::string current_groups = payload.value("user_groups", "");
            if (current_groups.find("admins") == std::string::npos && current_groups.find("lldap_admin") == std::string::npos) {
                return {{"status", "error"}, {"message", "Critical Action Requires Administrator Privileges."}};
            }

            try {
                std::string target_id = payload.at("target_id").get<std::string>();
                std::string conf1 = payload.value("confirm_1", "");
                std::string conf2 = payload.value("confirm_2", "");

                if (target_id == "global") return {{"status", "error"}, {"message", "Cannot hard-delete the master global vault."}};
                if (conf1 != "DELETE" || conf2 != target_id) {
                    return {{"status", "error"}, {"message", "Dual confirmation failed. You must type exactly as requested."}};
                }

                pqxx::connection C(std::getenv("OSL_DB_CONN"));
                pqxx::work W(C);

                W.exec("DELETE FROM landlord_user_mapping WHERE entity_id IN (SELECT entity_id FROM landlord_entities WHERE entity_id = " + W.quote(target_id) + " OR parent_id = " + W.quote(target_id) + ")");
                W.exec("DELETE FROM landlord_entities WHERE entity_id = " + W.quote(target_id) + " OR parent_id = " + W.quote(target_id));
                
                W.commit();
                return {{"status", "SUCCESS"}, {"message", "Entity and all dependencies recursively erased from the database."}};
            } catch (const std::exception &e) {
                std::string err_msg = e.what();
                if (err_msg.find("violates foreign key constraint") != std::string::npos) {
                     return {{"status", "error"}, {"message", "Hard Delete Blocked! Financial records exist in the Core Ledger for this entity. You must use Soft Deactivate instead to preserve the cryptographic audit trail."}};
                }
                return {{"status", "error"}, {"message", "Database error during hard deletion."}};
            }
        }

        if (command == "transfer_entity") {
            std::string current_groups = payload.value("user_groups", "");
            if (current_groups.find("admins") == std::string::npos && current_groups.find("lldap_admin") == std::string::npos) {
                return {{"status", "error"}, {"message", "Action Requires Administrator Privileges."}};
            }

            try {
                std::string target_id = payload.at("target_id").get<std::string>();
                std::string new_parent = payload.at("new_parent_id").get<std::string>();
                if (target_id == "global") return {{"status", "error"}, {"message", "Cannot transfer the master global vault."}};
                
                pqxx::connection C(std::getenv("OSL_DB_CONN"));
                pqxx::work W(C);
                if (new_parent.empty()) {
                    W.exec("UPDATE landlord_entities SET parent_id = NULL WHERE entity_id = " + W.quote(target_id));
                } else {
                    W.exec("UPDATE landlord_entities SET parent_id = " + W.quote(new_parent) + " WHERE entity_id = " + W.quote(target_id));
                }
                W.commit();
                return {{"status", "SUCCESS"}, {"message", "Trading Name Transferred Successfully."}};
            } catch (const std::exception &e) {
                return {{"status", "error"}, {"message", "Database error during transfer."}};
            }
        }

        if (command == "create_entity") {
            std::string current_groups = payload.value("user_groups", "");
            if (current_groups.find("admins") == std::string::npos && current_groups.find("lldap_admin") == std::string::npos) {
                return {{"status", "error"}, {"message", "Action Requires Administrator Privileges."}};
            }

            try {
                std::string e_id = payload.at("entity_id").get<std::string>();
                std::string e_name = payload.at("business_name").get<std::string>();
                std::string e_acn = payload.value("acn_abn", "");
                std::string e_parent = payload.value("parent_id", "");

                pqxx::connection C(std::getenv("OSL_DB_CONN"));
                pqxx::work W(C);

                if (e_parent.empty()) {
                    W.exec("INSERT INTO landlord_entities (entity_id, business_name, acn_abn, parent_id, is_active) VALUES (" +
                           W.quote(e_id) + ", " + W.quote(e_name) + ", " + W.quote(e_acn) + ", NULL, true) "
                           "ON CONFLICT (entity_id) DO UPDATE SET business_name = EXCLUDED.business_name, acn_abn = EXCLUDED.acn_abn, parent_id = NULL, is_active = true");
                } else {
                    W.exec("INSERT INTO landlord_entities (entity_id, business_name, acn_abn, parent_id, is_active) VALUES (" +
                           W.quote(e_id) + ", " + W.quote(e_name) + ", " + W.quote(e_acn) + ", " + W.quote(e_parent) + ", true) "
                           "ON CONFLICT (entity_id) DO UPDATE SET business_name = EXCLUDED.business_name, acn_abn = EXCLUDED.acn_abn, parent_id = EXCLUDED.parent_id, is_active = true");
                }
                W.commit();

                return {{"status", "SUCCESS"}, {"message", "Sovereign Tenancy Saved Successfully."}};
            } catch (const std::exception &e) {
                return {{"status", "error"}, {"message", "Database error saving tenancy."}};
            }
        }

        if (command == "add_user_mapping") {
            std::string current_groups = payload.value("user_groups", "");
            if (current_groups.find("admins") == std::string::npos && current_groups.find("lldap_admin") == std::string::npos && current_groups.find("accountants") == std::string::npos) {
                return {{"status", "error"}, {"message", "Action Requires Administrator or Accountant Privileges."}};
            }

            try {
                std::string target_uid = payload.at("target_uid").get<std::string>();
                std::string target_eid = payload.at("target_eid").get<std::string>();

                pqxx::connection C(std::getenv("OSL_DB_CONN"));
                pqxx::work W(C);

                W.exec("INSERT INTO landlord_user_mapping (lldap_uid, entity_id) VALUES (" + 
                       W.quote(target_uid) + ", " + W.quote(target_eid) + ") " + 
                       "ON CONFLICT (lldap_uid, entity_id) DO NOTHING;");
                W.commit();

                return {{"status", "success"}, {"message", "Sovereign Link Established successfully."}};
            } catch (const std::exception &e) {
                return {{"status", "error"}, {"message", "Database connection or schema error."}};
            }
        }

        if (command == "list_users") {
            std::string current_groups = payload.value("user_groups", "");
            if (current_groups.find("admins") == std::string::npos && current_groups.find("lldap_admin") == std::string::npos && current_groups.find("accountants") == std::string::npos) {
                return {{"status", "error"}, {"message", "Access Denied."}};
            }

            try {
                std::string admin_user = std::getenv("LLDAP_ADMIN_USER") ? std::getenv("LLDAP_ADMIN_USER") : "admin";
                std::string admin_pass = std::getenv("LLDAP_ADMIN_PASS") ? std::getenv("LLDAP_ADMIN_PASS") : "";

                httplib::Client lldap("http://osl-identity:17170");
                json auth_payload = {{"username", admin_user}, {"password", admin_pass}};
                auto auth_res = lldap.Post("/auth/simple/login", auth_payload.dump(), "application/json");

                if (!auth_res || auth_res->status != 200) return {{"status", "error"}, {"message", "Identity vault offline."}};

                std::string token = json::parse(auth_res->body)["token"];
                httplib::Headers headers = {{"Authorization", "Bearer " + token}};
                json query_payload = {{"query", "query { users { id email displayName firstName lastName groups { id displayName } } }"}};
                auto query_res = lldap.Post("/api/graphql", headers, query_payload.dump(), "application/json");

                return json::parse(query_res->body);
            } catch (const std::exception &e) {
                return {{"status", "error"}, {"message", "Failed to retrieve user list."}};
            }
        }

        if (command == "add_user" || command == "update_user") {
            std::string current_groups = payload.value("user_groups", "");
            if (current_groups.find("admins") == std::string::npos && current_groups.find("lldap_admin") == std::string::npos) {
                return {{"status", "error"}, {"message", "Action Requires Administrator Privileges."}};
            }

            try {
                std::string target_user = payload.at("username").get<std::string>();
                std::string new_email = payload.at("email").get<std::string>();
                std::string new_display = payload.at("displayName").get<std::string>();
                std::string new_first = payload.at("firstName").get<std::string>();
                std::string new_last = payload.at("lastName").get<std::string>();
                std::string new_pass = payload.value("password", "");
                
                std::string target_role = "clients";
                if (payload.contains("role")) target_role = payload.at("role").get<std::string>();
                else if (payload.contains("roleId")) target_role = payload.at("roleId").get<std::string>();

                std::string admin_user = std::getenv("LLDAP_ADMIN_USER") ? std::getenv("LLDAP_ADMIN_USER") : "admin";
                std::string admin_pass = std::getenv("LLDAP_ADMIN_PASS") ? std::getenv("LLDAP_ADMIN_PASS") : "";
                
                httplib::Client lldap("http://osl-identity:17170");
                json auth_payload = {{"username", admin_user}, {"password", admin_pass}};
                auto auth_res = lldap.Post("/auth/simple/login", auth_payload.dump(), "application/json");
                if (!auth_res || auth_res->status != 200) return {{"status", "error"}, {"message", "Identity vault offline."}};
                
                std::string token = json::parse(auth_res->body)["token"];
                httplib::Headers headers = {{"Authorization", "Bearer " + token}};

                if (command == "add_user") {
                    json create_payload = {
                        {"query", "mutation CreateUser($user: CreateUserInput!) { createUser(user: $user) { id } }"},
                        {"variables", {{"user", {{"id", target_user}, {"email", new_email}, {"displayName", new_display}, {"firstName", new_first}, {"lastName", new_last}}}}}
                    };
                    auto create_res = lldap.Post("/api/graphql", headers, create_payload.dump(), "application/json");
                    if (create_res && json::parse(create_res->body).contains("errors")) {
                        return {{"status", "error"}, {"message", json::parse(create_res->body)["errors"][0]["message"].get<std::string>()}};
                    }
                    if (!set_lldap_password(admin_user, admin_pass, target_user, new_pass)) {
                        return {{"status", "error"}, {"message", "User created, but password sync failed."}};
                    }
                } else { 
                    json update_payload = {
                        {"query", "mutation UpdateUser($user: UpdateUserInput!) { updateUser(user: $user) { ok } }"},
                        {"variables", {{"user", {{"id", target_user}, {"email", new_email}, {"displayName", new_display}, {"firstName", new_first}, {"lastName", new_last}}}}}
                    };
                    auto update_res = lldap.Post("/api/graphql", headers, update_payload.dump(), "application/json");
                    if (update_res && json::parse(update_res->body).contains("errors")) {
                        return {{"status", "error"}, {"message", "User Update Failed."}};
                    }
                    if (!new_pass.empty() && !set_lldap_password(admin_user, admin_pass, target_user, new_pass)) {
                        return {{"status", "error"}, {"message", "Password Update Failed."}};
                    }
                }

                json group_query = {{"query", "query { groups { id displayName } }"}};
                auto group_res = lldap.Post("/api/graphql", headers, group_query.dump(), "application/json");
                int resolved_group_id = -1;
                
                if (group_res && group_res->status == 200) {
                    auto existing_data = json::parse(group_res->body);
                    if (existing_data.contains("data") && existing_data["data"].contains("groups")) {
                        for (const auto& g : existing_data["data"]["groups"]) {
                            if (g["displayName"] == target_role) {
                                resolved_group_id = g["id"].get<int>(); break;
                            }
                        }
                    }
                }

                if (resolved_group_id != -1) {
                    json assign_payload = {
                        {"query", "mutation AddUserToGroup($userId: String!, $groupId: Int!) { addUserToGroup(userId: $userId, groupId: $groupId) { ok } }"},
                        {"variables", { {"userId", target_user}, {"groupId", resolved_group_id} }}
                    };
                    lldap.Post("/api/graphql", headers, assign_payload.dump(), "application/json");
                }

                return {{"status", "SUCCESS"}, {"message", "User Synced & Assigned to Role: " + target_role}};
            } catch (const std::exception &e) {
                return {{"status", "error"}, {"message", "Payload missing required fields."}};
            }
        }

        return {{"status", "active"}};
    }
};

extern "C" IOSLPlugin* create_plugin() {
    return new LandlordPlugin();
}