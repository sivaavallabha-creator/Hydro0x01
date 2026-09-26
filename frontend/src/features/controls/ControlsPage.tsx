import { useState } from 'react';
import { useSystemStore } from '../../store/useSystemStore';
import { controlPump, controlMode, sendEnvCommand } from '../../services/api';
import { toast } from 'sonner';
import {
  Power, Fan, Sun, Droplets, Settings2, Loader2,
} from 'lucide-react';

export default function ControlsPage() {
  const { selectedDevice, status } = useSystemStore();
  const [loading, setLoading] = useState<string | null>(null);
  const [pumpDuration, setPumpDuration] = useState(30);

  const exec = async (id: string, action: () => Promise<any>, successMsg: string) => {
    if (!selectedDevice) { toast.error('No device selected'); return; }
    setLoading(id);
    try {
      await action();
      toast.success(successMsg);
    } catch (e: any) {
      toast.error(e.message || 'Command failed');
    } finally {
      setLoading(null);
    }
  };

  return (
    <div>
      <div style={{ marginBottom: 32 }}>
        <h1 style={{ fontSize: 28, marginBottom: 4 }}>Controls</h1>
        <p style={{ color: 'var(--text-muted)', fontSize: 14 }}>
          Manual overrides for <strong style={{ color: 'var(--text-primary)' }}>{selectedDevice || '—'}</strong>
        </p>
      </div>

      <div className="controls-grid">
        {/* Pump Control */}
        <div className="card control-section">
          <h3 className="control-section-title">
            <Droplets size={18} /> Irrigation
          </h3>

          <div className="control-row">
            <label className="input-label">Pump Duration (seconds)</label>
            <input
              type="number"
              className="input"
              value={pumpDuration}
              onChange={e => setPumpDuration(Number(e.target.value))}
              min={1}
              max={600}
              style={{ maxWidth: 160 }}
            />
          </div>

          <div className="control-buttons">
            <button
              className="btn btn-primary"
              disabled={loading === 'pump_on'}
              onClick={() => exec('pump_on', () => controlPump(selectedDevice, 'on', pumpDuration * 1000), 'Pump started')}
            >
              {loading === 'pump_on' ? <Loader2 size={16} className="animate-spin" /> : <Power size={16} />}
              Pump ON
            </button>
            <button
              className="btn btn-danger"
              disabled={loading === 'pump_off'}
              onClick={() => exec('pump_off', () => controlPump(selectedDevice, 'off'), 'Pump stopped')}
            >
              {loading === 'pump_off' ? <Loader2 size={16} className="animate-spin" /> : <Power size={16} />}
              Pump OFF
            </button>
          </div>

          <div className="control-status">
            <span>Current: </span>
            <span className={`badge ${status.pumpState === 'ON' ? 'badge-success' : status.pumpState === 'COOLDOWN' ? 'badge-warning' : 'badge-neutral'}`}>
              {status.pumpState}
            </span>
          </div>
        </div>

        {/* Environment Controls */}
        <div className="card control-section">
          <h3 className="control-section-title">
            <Sun size={18} /> Environment
          </h3>

          <div className="control-buttons-grid">
            <ControlButton
              id="light_on" label="Light ON" icon={Sun} color="var(--warning)"
              loading={loading} onClick={() => exec('light_on', () => sendEnvCommand(selectedDevice, 'light_on'), 'Light turned ON')}
            />
            <ControlButton
              id="light_off" label="Light OFF" icon={Sun} color="var(--text-dimmed)"
              loading={loading} onClick={() => exec('light_off', () => sendEnvCommand(selectedDevice, 'light_off'), 'Light turned OFF')}
            />
            <ControlButton
              id="light_auto" label="Light AUTO" icon={Sun} color="var(--accent)"
              loading={loading} onClick={() => exec('light_auto', () => sendEnvCommand(selectedDevice, 'light_auto'), 'Light set to AUTO')}
            />
            <ControlButton
              id="fan_on" label="Fan ON" icon={Fan} color="var(--info)"
              loading={loading} onClick={() => exec('fan_on', () => sendEnvCommand(selectedDevice, 'fan_on'), 'Fan turned ON')}
            />
            <ControlButton
              id="fan_off" label="Fan OFF" icon={Fan} color="var(--text-dimmed)"
              loading={loading} onClick={() => exec('fan_off', () => sendEnvCommand(selectedDevice, 'fan_off'), 'Fan turned OFF')}
            />
            <ControlButton
              id="fan_auto" label="Fan AUTO" icon={Fan} color="var(--accent)"
              loading={loading} onClick={() => exec('fan_auto', () => sendEnvCommand(selectedDevice, 'fan_auto'), 'Fan set to AUTO')}
            />
          </div>
        </div>

        {/* Operating Mode */}
        <div className="card control-section">
          <h3 className="control-section-title">
            <Settings2 size={18} /> Operating Mode
          </h3>

          <div className="control-buttons">
            {(['active', 'maintenance'] as const).map(mode => (
              <button
                key={mode}
                className={`btn ${status.state === mode.toUpperCase() || (mode === 'active' && status.state === 'ACTIVE') ? 'btn-primary' : 'btn-secondary'}`}
                disabled={loading === `mode_${mode}`}
                onClick={() => exec(`mode_${mode}`, () => controlMode(selectedDevice, mode), `Mode set to ${mode}`)}
              >
                {loading === `mode_${mode}` ? <Loader2 size={16} className="animate-spin" /> : null}
                {mode.charAt(0).toUpperCase() + mode.slice(1)}
              </button>
            ))}
          </div>

          <div className="control-status">
            <span>Current: </span>
            <span className={`badge ${status.state === 'ACTIVE' ? 'badge-success' : status.state === 'MAINTENANCE' ? 'badge-warning' : 'badge-neutral'}`}>
              {status.state}
            </span>
          </div>
        </div>
      </div>

      <style>{`
        .controls-grid {
          display: grid;
          grid-template-columns: repeat(auto-fit, minmax(340px, 1fr));
          gap: 20px;
        }

        .control-section {
          padding: 24px;
          display: flex;
          flex-direction: column;
          gap: 20px;
        }

        .control-section-title {
          display: flex;
          align-items: center;
          gap: 8px;
          font-size: 16px;
          font-weight: 600;
          margin: 0;
          color: var(--text-primary);
        }

        .control-row {
          display: flex;
          flex-direction: column;
          gap: 4px;
        }

        .control-buttons {
          display: flex;
          flex-wrap: wrap;
          gap: 10px;
        }

        .control-buttons-grid {
          display: grid;
          grid-template-columns: repeat(auto-fill, minmax(130px, 1fr));
          gap: 10px;
        }

        .control-status {
          display: flex;
          align-items: center;
          gap: 8px;
          font-size: 13px;
          color: var(--text-muted);
          padding-top: 4px;
          border-top: 1px solid var(--border-default);
        }

        .control-tile {
          all: unset;
          box-sizing: border-box;
          cursor: pointer;
          display: flex;
          flex-direction: column;
          align-items: center;
          justify-content: center;
          gap: 8px;
          padding: 16px 12px;
          border-radius: var(--radius-md);
          border: 1px solid var(--border-default);
          background: var(--bg-card);
          transition: all var(--transition-fast);
          text-align: center;
        }

        .control-tile:hover:not(:disabled) {
          border-color: var(--border-strong);
          background: var(--bg-card-hover);
        }

        .control-tile:disabled {
          opacity: 0.4;
          cursor: not-allowed;
        }

        .control-tile-label {
          font-size: 12px;
          font-weight: 600;
          color: var(--text-secondary);
        }
      `}</style>
    </div>
  );
}

function ControlButton({ id, label, icon: Icon, color, loading, onClick }: {
  id: string; label: string; icon: any; color: string; loading: string | null; onClick: () => void;
}) {
  return (
    <button className="control-tile" disabled={loading === id} onClick={onClick}>
      {loading === id ? <Loader2 size={22} className="animate-spin" style={{ color }} /> : <Icon size={22} style={{ color }} />}
      <span className="control-tile-label">{label}</span>
    </button>
  );
}
