import { useMemo } from 'react';
import { useTelemetryStore } from '../../store/useTelemetryStore';
import {
  LineChart, Line, XAxis, YAxis, CartesianGrid, Tooltip, ResponsiveContainer, Area, AreaChart,
} from 'recharts';
import { Thermometer, Wind, Gauge, Sun, Fan } from 'lucide-react';
import { request } from '../../services/api';
import { useSystemStore } from '../../store/useSystemStore';
import { toast } from 'sonner';

const CustomTooltip = ({ active, payload, label }: any) => {
  if (!active || !payload?.length) return null;
  return (
    <div style={{
      background: 'var(--bg-card)',
      border: '1px solid var(--border-default)',
      borderRadius: 'var(--radius-md)',
      padding: '10px 14px',
      boxShadow: 'var(--shadow-lg)',
    }}>
      <p style={{ color: 'var(--text-muted)', fontSize: 12, marginBottom: 6 }}>{label}</p>
      {payload.map((entry: any, i: number) => (
        <p key={i} style={{ color: entry.color, fontSize: 13, fontWeight: 600, margin: '2px 0' }}>
          {entry.name}: {typeof entry.value === 'number' ? entry.value.toFixed(1) : entry.value}
        </p>
      ))}
    </div>
  );
};

export default function EnvironmentPage() {
  const { history } = useTelemetryStore();
  const { selectedDevice } = useSystemStore();

  const sendEnvCommand = async (action: string) => {
    if (!selectedDevice) return toast.error('No device selected');
    try {
      await request('/api/control/env', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ deviceId: selectedDevice, action })
      });
      toast.success(`Sent command: ${action.replace('_', ' ').toUpperCase()}`);
    } catch {
      toast.error('Failed to send environment command');
    }
  };

  const chartData = useMemo(() => {
    return history.slice(-60).map((h, i) => ({
      idx: i,
      airTemp: h.air_temperature ?? h.air_temp,
      waterTemp: h.water_temperature ?? h.water_temp,
      humidity: h.air_humidity ?? h.humidity,
      pressure: h.air_pressure ?? h.pressure,
      waterLevel: h.water_level ?? h.water_level_percent,
    }));
  }, [history]);

  const chartStyle = {
    cartesian: { strokeDasharray: '3 3', stroke: 'var(--border-default)', vertical: false as const },
    xAxis: { stroke: 'var(--text-dimmed)', fontSize: 11, tickMargin: 8 },
    yAxis: { stroke: 'var(--text-dimmed)', fontSize: 11 },
  };

  const StatCard = ({ icon: Icon, label, value, unit, color }: any) => (
    <div className="card" style={{ padding: 20 }}>
      <div style={{ display: 'flex', alignItems: 'center', gap: 10, marginBottom: 8 }}>
        <Icon size={16} style={{ color }} />
        <span style={{ fontSize: 13, color: 'var(--text-muted)', fontWeight: 500 }}>{label}</span>
      </div>
      <div style={{ fontSize: 32, fontWeight: 700, letterSpacing: '-0.03em' }}>
        {value !== undefined ? (typeof value === 'number' ? value.toFixed(1) : value) : '—'}
        <span style={{ fontSize: 14, fontWeight: 500, color: 'var(--text-muted)', marginLeft: 4 }}>{unit}</span>
      </div>
    </div>
  );

  const latest = chartData[chartData.length - 1];

  return (
    <div>
      <div style={{ marginBottom: 32 }}>
        <h1 style={{ fontSize: 28, marginBottom: 4 }}>Environment</h1>
        <p style={{ color: 'var(--text-muted)', fontSize: 14 }}>Real-time environmental monitoring and analytics</p>
      </div>

      {/* Quick Stats */}
      <div style={{ display: 'grid', gridTemplateColumns: 'repeat(auto-fill, minmax(200px, 1fr))', gap: 16, marginBottom: 28 }}>
        <StatCard icon={Thermometer} label="Air Temperature" value={latest?.airTemp} unit="°C" color="var(--chart-1)" />
        <StatCard icon={Thermometer} label="Water Temperature" value={latest?.waterTemp} unit="°C" color="var(--chart-2)" />
        <StatCard icon={Wind} label="Humidity" value={latest?.humidity} unit="%" color="var(--chart-3)" />
        <StatCard icon={Gauge} label="Pressure" value={latest?.pressure} unit="hPa" color="var(--chart-4)" />
      </div>

      {/* Manual Controls */}
      <div className="card animate-fade-in stagger-1" style={{ padding: 20, marginBottom: 20 }}>
        <h3 style={{ fontSize: 15, fontWeight: 600, marginBottom: 16, color: 'var(--text-primary)' }}>Manual Actuator Overrides</h3>
        <div style={{ display: 'flex', gap: 24, flexWrap: 'wrap' }}>
          
          <div style={{ flex: 1, minWidth: 250, background: 'var(--bg-input)', padding: 16, borderRadius: 'var(--radius-md)', border: '1px solid var(--border-default)' }}>
            <div style={{ display: 'flex', alignItems: 'center', gap: 8, marginBottom: 12, color: 'var(--chart-3)' }}>
              <Fan size={18} />
              <span style={{ fontWeight: 600, fontSize: 14 }}>Exhaust Fan</span>
            </div>
            <div style={{ display: 'flex', gap: 8 }}>
              <button className="btn btn-secondary" style={{ flex: 1 }} onClick={() => sendEnvCommand('fan_on')}>ON</button>
              <button className="btn btn-secondary" style={{ flex: 1 }} onClick={() => sendEnvCommand('fan_off')}>OFF</button>
              <button className="btn" style={{ flex: 1, background: 'var(--bg-card)', border: '1px solid var(--border-strong)' }} onClick={() => sendEnvCommand('fan_auto')}>AUTO</button>
            </div>
          </div>

          <div style={{ flex: 1, minWidth: 250, background: 'var(--bg-input)', padding: 16, borderRadius: 'var(--radius-md)', border: '1px solid var(--border-default)' }}>
            <div style={{ display: 'flex', alignItems: 'center', gap: 8, marginBottom: 12, color: 'var(--chart-1)' }}>
              <Sun size={18} />
              <span style={{ fontWeight: 600, fontSize: 14 }}>Grow Lights</span>
            </div>
            <div style={{ display: 'flex', gap: 8 }}>
              <button className="btn btn-secondary" style={{ flex: 1 }} onClick={() => sendEnvCommand('light_on')}>ON</button>
              <button className="btn btn-secondary" style={{ flex: 1 }} onClick={() => sendEnvCommand('light_off')}>OFF</button>
              <button className="btn" style={{ flex: 1, background: 'var(--bg-card)', border: '1px solid var(--border-strong)' }} onClick={() => sendEnvCommand('light_auto')}>AUTO</button>
            </div>
          </div>

        </div>
      </div>

      {/* Charts */}
      <div style={{ display: 'flex', flexDirection: 'column', gap: 20 }}>
        {/* Temperature Area Chart */}
        <div className="card animate-fade-in" style={{ padding: 20 }}>
          <h3 style={{ fontSize: 15, fontWeight: 600, marginBottom: 16, marginLeft: 4 }}>Temperature Live Feed</h3>
          <div style={{ height: 260, width: '100%' }}>
            <ResponsiveContainer>
              <AreaChart data={chartData} margin={{ top: 5, right: 20, left: 0, bottom: 5 }}>
                <defs>
                  <linearGradient id="gradAir" x1="0" y1="0" x2="0" y2="1">
                    <stop offset="5%" stopColor="var(--chart-1)" stopOpacity={0.3} />
                    <stop offset="95%" stopColor="var(--chart-1)" stopOpacity={0} />
                  </linearGradient>
                  <linearGradient id="gradWater" x1="0" y1="0" x2="0" y2="1">
                    <stop offset="5%" stopColor="var(--chart-2)" stopOpacity={0.3} />
                    <stop offset="95%" stopColor="var(--chart-2)" stopOpacity={0} />
                  </linearGradient>
                </defs>
                <CartesianGrid {...chartStyle.cartesian} />
                <XAxis dataKey="idx" {...chartStyle.xAxis} />
                <YAxis {...chartStyle.yAxis} domain={['auto', 'auto']} />
                <Tooltip content={<CustomTooltip />} />
                <Area type="monotone" name="Air Temp" dataKey="airTemp" stroke="var(--chart-1)" fill="url(#gradAir)" strokeWidth={2} isAnimationActive={false} />
                <Area type="monotone" name="Water Temp" dataKey="waterTemp" stroke="var(--chart-2)" fill="url(#gradWater)" strokeWidth={2} isAnimationActive={false} />
              </AreaChart>
            </ResponsiveContainer>
          </div>
        </div>

        {/* Humidity & Water Level */}
        <div className="card animate-fade-in stagger-2" style={{ padding: 20 }}>
          <h3 style={{ fontSize: 15, fontWeight: 600, marginBottom: 16, marginLeft: 4 }}>Humidity & Water Level</h3>
          <div style={{ height: 260, width: '100%' }}>
            <ResponsiveContainer>
              <LineChart data={chartData} margin={{ top: 5, right: 20, left: 0, bottom: 5 }}>
                <CartesianGrid {...chartStyle.cartesian} />
                <XAxis dataKey="idx" {...chartStyle.xAxis} />
                <YAxis {...chartStyle.yAxis} domain={[0, 100]} />
                <Tooltip content={<CustomTooltip />} />
                <Line type="monotone" name="Humidity (%)" dataKey="humidity" stroke="var(--chart-3)" strokeWidth={2} dot={false} isAnimationActive={false} />
                <Line type="monotone" name="Water Level (%)" dataKey="waterLevel" stroke="var(--chart-6)" strokeWidth={2} dot={false} isAnimationActive={false} />
              </LineChart>
            </ResponsiveContainer>
          </div>
        </div>

        {/* Pressure */}
        <div className="card animate-fade-in stagger-3" style={{ padding: 20 }}>
          <h3 style={{ fontSize: 15, fontWeight: 600, marginBottom: 16, marginLeft: 4 }}>Barometric Pressure</h3>
          <div style={{ height: 220, width: '100%' }}>
            <ResponsiveContainer>
              <AreaChart data={chartData} margin={{ top: 5, right: 20, left: 0, bottom: 5 }}>
                <defs>
                  <linearGradient id="gradPressure" x1="0" y1="0" x2="0" y2="1">
                    <stop offset="5%" stopColor="var(--chart-4)" stopOpacity={0.3} />
                    <stop offset="95%" stopColor="var(--chart-4)" stopOpacity={0} />
                  </linearGradient>
                </defs>
                <CartesianGrid {...chartStyle.cartesian} />
                <XAxis dataKey="idx" {...chartStyle.xAxis} />
                <YAxis {...chartStyle.yAxis} domain={['auto', 'auto']} />
                <Tooltip content={<CustomTooltip />} />
                <Area type="monotone" name="Pressure (hPa)" dataKey="pressure" stroke="var(--chart-4)" fill="url(#gradPressure)" strokeWidth={2} isAnimationActive={false} />
              </AreaChart>
            </ResponsiveContainer>
          </div>
        </div>
      </div>
    </div>
  );
}
