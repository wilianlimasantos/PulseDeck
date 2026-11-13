//! PulseDeck core: everything that talks to the pedal or converts files, independent of the GUI.

pub mod a2l;
pub mod backup;
pub mod ble;
pub mod globals;
pub mod ir;
pub mod link;
pub mod manager;
pub mod nam;
pub mod parse;
pub mod protocol;
pub mod rig;
pub mod session;
pub mod sim;
pub mod upload;

#[cfg(test)]
mod integration_tests;
